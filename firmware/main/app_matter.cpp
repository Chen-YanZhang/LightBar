// app_matter.cpp - Matter device: ColorTemperatureLight endpoint (OnOff +
// LevelControl + ColorControl/CT feature) fed into the safety-gated output
// stage. Built on esp_matter 1.6 legacy data model (default, CONFIG off).
#include <esp_matter.h>
#include <app/server/Server.h>
#include <credentials/FabricTable.h>
#include <platform/ESP32/NetworkCommissioningDriver.h>
#include <platform/ESP32/ESP32Utils.h>
#include <platform/PlatformManager.h>
#include <platform/ConnectivityManager.h>
#include <esp_log.h>
#include <platform/CommissionableDataProvider.h>
#include "app_matter.h"
#include "lightbar_out.h"
#include "lightbar_mix.h"
#include "rgb_status.h"
#include "solar_policy.h"
#include "evlog.h"
#include "esp_matter_ota.h"
#include <setup_payload/SetupPayload.h>
#include <setup_payload/ManualSetupPayloadGenerator.h>
#include <setup_payload/QRCodeSetupPayloadGenerator.h>
#include <string>
#include <app/clusters/mode-select-server/SupportedModesManager.h>
#include <app-common/zap-generated/cluster-objects.h>

using namespace esp_matter;
using namespace esp_matter::endpoint;

static const char *TAG = "lb-matter";

/* Cluster/attribute ids spelled numerically on purpose: stable across the
 * connectedhomeip header reshuffles that esp_matter 1.6 sits on. */
static constexpr uint32_t CLUSTER_ON_OFF = 0x0006;
static constexpr uint32_t ATTR_ON_OFF = 0x0000;
static constexpr uint32_t CLUSTER_LEVEL_CONTROL = 0x0008;
/* Matter LevelControl remaps ZCL ids: CurrentLevel = 0x0000 (0x0011 is OnLevel).
 * Confirmed on hardware: boot init posts cluster 0x0008 / attr 0x0000. */
static constexpr uint32_t ATTR_CURRENT_LEVEL = 0x0000;
static constexpr uint32_t CLUSTER_COLOR_CONTROL = 0x0300;
static constexpr uint32_t ATTR_COLOR_TEMP_MIREDS = 0x0007;

static bool s_on;
static uint8_t s_level = 200;
static uint16_t s_mireds = LB_CT_WARM_MIREDS;
static uint16_t s_endpoint_id;
static bool s_syncing;

/* ---- Mode Select (0x0050): "Normal" / "Diagnostic" ------------------------
 * The only standard way to show the bench diagnostic mode in a Matter
 * controller. Home Assistant renders it as a select entity on the device;
 * switching it to Normal exits diagnostic drive, the web toggle moves it. */
#define CLUSTER_MODE_SELECT 0x0050u
#define ATTR_CURRENT_MODE   0x0003u
#define LB_MODE_NORMAL      0u
#define LB_MODE_DIAG        1u

namespace {
using chip::app::Clusters::ModeSelect::SupportedModesManager;
using ModeOption = chip::app::Clusters::ModeSelect::Structs::ModeOptionStruct::Type;
using SemanticTag = chip::app::Clusters::ModeSelect::Structs::SemanticTagStruct::Type;

class LbModesManager : public SupportedModesManager {
public:
    ModeOptionsProvider getModeOptionsProvider(chip::EndpointId ep) const override
    {
        if (ep != s_endpoint_id) return ModeOptionsProvider();
        return ModeOptionsProvider(&kModes[0], &kModes[0] + kCount);
    }
    chip::Protocols::InteractionModel::Status getModeOptionByMode(chip::EndpointId ep, uint8_t mode,
                                                                   const ModeOption **out) const override
    {
        if (ep != s_endpoint_id) return chip::Protocols::InteractionModel::Status::UnsupportedEndpoint;
        for (size_t i = 0; i < kCount; i++) {
            if (kModes[i].mode == mode) { *out = &kModes[i]; return chip::Protocols::InteractionModel::Status::Success; }
        }
        return chip::Protocols::InteractionModel::Status::InvalidCommand;
    }
private:
    static constexpr size_t kCount = 2;
    static const ModeOption kModes[kCount];
};
const ModeOption LbModesManager::kModes[kCount] = {
    { .label = chip::CharSpan::fromCharString("Normal"),     .mode = LB_MODE_NORMAL, .semanticTags = chip::app::DataModel::List<const SemanticTag>() },
    { .label = chip::CharSpan::fromCharString("Diagnostic"), .mode = LB_MODE_DIAG,   .semanticTags = chip::app::DataModel::List<const SemanticTag>() },
};
LbModesManager s_modes_mgr;
} // namespace
static esp_err_t attribute_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                              uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    (void)endpoint_id;
    (void)priv_data;

    if (type != attribute::POST_UPDATE) {
        return ESP_OK; /* PRE_UPDATE: allow; commands update the DB, we act after */
    }

    /* CurrentLevel is nullable in Matter, so compare storage types (nullable
     * bit stripped, enum/bitmap normalized) and reject null payloads. */
    if (val->is_null()) {
        return ESP_OK;
    }

    if (cluster_id == CLUSTER_MODE_SELECT && attribute_id == ATTR_CURRENT_MODE) {
        if (s_syncing || val->get_storage_type() != ESP_MATTER_VAL_TYPE_UINT8) {
            return ESP_OK;
        }
        lb_diag_drive_t d;
        lightbar_out_get_diag(&d);
        bool want = (val->val.u8 == LB_MODE_DIAG);
        evlog("matter", "mode select -> %s", want ? "Diagnostic" : "Normal");
        if (want != d.on) {
            d.on = want;
            lightbar_out_set_diag(&d);   /* reports the resulting light state back (s_syncing) */
        }
        return ESP_OK;
    }

    bool handled = false;
    bool ct_changed = false;
    switch (cluster_id) {
    case CLUSTER_ON_OFF:
        if (attribute_id == ATTR_ON_OFF && val->get_storage_type() == ESP_MATTER_VAL_TYPE_BOOLEAN) {
            s_on = val->val.b;
            handled = true;
        }
        break;
    case CLUSTER_LEVEL_CONTROL:
        if (attribute_id == ATTR_CURRENT_LEVEL && val->get_storage_type() == ESP_MATTER_VAL_TYPE_UINT8) {
            s_level = val->val.u8;
            handled = true;
        }
        break;
    case CLUSTER_COLOR_CONTROL:
        if (attribute_id == ATTR_COLOR_TEMP_MIREDS && val->get_storage_type() == ESP_MATTER_VAL_TYPE_UINT16) {
            s_mireds = val->val.u16;
            handled = true;
            ct_changed = true;
        }
        break;
    }

    if (!handled) {
        return ESP_OK;
    }

    /* attribute::update() fires this callback synchronously. When the write came
     * from lb_matter_sync_target() (web page / sun curve -> Matter) the shadow
     * copies above are all we want: the output is already set, and a colour
     * published BY the sun curve must not be mistaken for a manual one (that
     * used to switch sun-following off on its own first update). */
    if (s_syncing) {
        return ESP_OK;
    }
    if (ct_changed) {
        /* A hand-set colour beats the sun curve until the user turns it back on. */
        solar_policy_note_manual_ct();
    }

    ESP_LOGI(TAG, "attr ep=%u cluster=0x%04X attr=0x%04X -> on=%d level=%u mireds=%u", endpoint_id,
             (unsigned)cluster_id, (unsigned)attribute_id, s_on, s_level, s_mireds);
    evlog("matter", "cl=%04X attr=%04X -> on=%d lvl=%u mi=%u", (unsigned)cluster_id, (unsigned)attribute_id,
          s_on, s_level, s_mireds);
    lightbar_out_set(s_on, s_level, s_mireds);
    /* A light command from the controller always wins over the bench tool:
     * leave diagnostic drive, the command above becomes the output. */
    lb_diag_drive_t d;
    lightbar_out_get_diag(&d);
    if (d.on) {
        evlog("matter", "light command while in diagnostic mode -> back to Normal");
        lightbar_out_set_diag(NULL);
    }
    return ESP_OK;
}

static esp_err_t identify_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                             uint8_t effect_variant, void *priv_data)
{
    (void)priv_data;
    /* M4: blink the WS2812 strip for identification. For now, log only. */
    ESP_LOGI(TAG, "identify ep=%u type=%d effect=%u variant=%u", endpoint_id, (int)type, effect_id, effect_variant);
    return ESP_OK;
}

static void matter_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t arg)
{
    (void)arg;
    using namespace chip::DeviceLayer::DeviceEventType;
    switch (event->Type) {
    case kCommissioningComplete:
        ESP_LOGI(TAG, "commissioning complete (fabric %u)", (unsigned)event->CommissioningComplete.fabricIndex);
        evlog("matter", "commissioning complete fabric=%u", (unsigned)event->CommissioningComplete.fabricIndex);
        break;
    case kFabricRemoved:
        ESP_LOGW(TAG, "fabric removed");
        evlog("matter", "fabric removed");
        break;
    case kCommissioningWindowOpened:
        ESP_LOGI(TAG, "commissioning window opened (BLE pairing active)");
        evlog("matter", "commissioning window opened");
        rgb_status_note_pairing(true);
        break;
    case kCommissioningWindowClosed:
        ESP_LOGI(TAG, "commissioning window closed");
        evlog("matter", "commissioning window closed");
        rgb_status_note_pairing(false);
        break;
    case kServerReady:
        evlog("matter", "server ready (fabrics=%u)", (unsigned)chip::Server::GetInstance().GetFabricTable().FabricCount());
        break;
    case kSecureSessionEstablished:
        /* A controller (HA) re-established a CASE session: from here on it can
         * control the lamp again. This is what ends the "unavailable" window. */
        evlog("matter", "session established type=%u peer=%08X", (unsigned)event->SecureSessionEstablished.SecureSessionType,
              (unsigned)(event->SecureSessionEstablished.PeerNodeId & 0xFFFFFFFFu));
        break;
    case kInternetConnectivityChange:
        evlog("matter", "internet ipv4=%d ipv6=%d", (int)event->InternetConnectivityChange.IPv4,
              (int)event->InternetConnectivityChange.IPv6);
        break;
    case kCHIPoBLEConnectionEstablished:
        evlog("matter", "BLE connection (commissioner)");
        break;
    case kOtaStateChanged:
        evlog("matter", "OTA requestor state changed");
        break;
    default:
        ESP_LOGD(TAG, "matter event type=%d", (int)event->Type);
        break;
    }
}

void lightbar_matter_start(void)
{
    node::config_t node_cfg;
    node_t *node = node::create(&node_cfg, attribute_cb, identify_cb);
    if (node == nullptr) {
        ESP_LOGE(TAG, "node create failed");
        return;
    }

    color_temperature_light::config_t lcfg;
    lcfg.on_off.on_off = false;
    lcfg.level_control.current_level = s_level;
    lcfg.level_control.min_level = 1;
    lcfg.level_control.max_level = 254;
    /* ColorCapabilities: do NOT set by hand. color_temperature::add() ORs in
     * kColorTemperature (0x10). The old value 0x0008 was bit3 = XY, which made
     * the device claim an XY feature it has no attributes for.
     * ColorMode / EnhancedColorMode for a CT-only light are 2
     * (ColorTemperatureMireds); the esp_matter defaults are 1 (hue/sat). */
    lcfg.color_control.color_capabilities = 0;
    lcfg.color_control.color_mode = 2;
    lcfg.color_control.enhanced_color_mode = 2;
    lcfg.color_control_color_temperature.color_temperature_mireds = s_mireds;
    lcfg.color_control_color_temperature.color_temp_physical_min_mireds = LB_CT_COLD_MIREDS;
    lcfg.color_control_color_temperature.color_temp_physical_max_mireds = LB_CT_WARM_MIREDS;
    /* Power-on applies the StartUp* attribute and silently overrode our warm
     * default with the ctor default (250 mireds) - set it explicitly. */
    lcfg.color_control_color_temperature.start_up_color_temperature_mireds = s_mireds;

    endpoint_t *ep = color_temperature_light::create(node, &lcfg, 0, nullptr);
    if (ep == nullptr) {
        ESP_LOGE(TAG, "color_temperature_light endpoint create failed");
        return;
    }
    s_endpoint_id = endpoint::get_id(ep);
    {
        cluster::mode_select::config_t mcfg;
        strncpy(mcfg.description, "Operating mode", sizeof(mcfg.description) - 1);
        mcfg.current_mode = LB_MODE_NORMAL;
        mcfg.delegate = &s_modes_mgr;
        if (cluster::mode_select::create(ep, &mcfg, CLUSTER_FLAG_SERVER) == nullptr) {
            ESP_LOGE(TAG, "mode_select cluster create failed");
        }
    }
    ESP_LOGI(TAG, "Matter endpoint %u created (device 0x010C ColorTemperatureLight)",
             (unsigned)s_endpoint_id);

    /* OTA Requestor (cluster 0x0019) on the root endpoint. This is what makes a
     * Matter controller offer firmware updates: Home Assistant's Matter server
     * can push a manually uploaded .ota image with developer mode enabled, so no
     * CSA/DCL registration is needed. Requires CONFIG_ENABLE_OTA_REQUESTOR. */
    esp_err_t ota_e = esp_matter_ota_requestor_init();
    if (ota_e == ESP_OK) {
        ESP_LOGI(TAG, "OTA Requestor added to root endpoint");
    } else {
        ESP_LOGW(TAG, "OTA Requestor unavailable: %s (CONFIG_ENABLE_OTA_REQUESTOR?)",
                 esp_err_to_name(ota_e));
    }

    esp_err_t e = esp_matter::start(matter_event_cb);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_matter::start failed: %s", esp_err_to_name(e));
        return;
    }
    ESP_LOGI(TAG, "Matter stack started; uncommissioned -> BLE advertising + onboarding payload in logs");

    chip::DeviceLayer::CommissionableDataProvider *cdp = chip::DeviceLayer::GetCommissionableDataProvider();
    if (cdp != nullptr) {
        uint16_t disc = 0;
        uint32_t passcode = 0;
        (void)cdp->GetSetupDiscriminator(disc);
        if (cdp->GetSetupPasscode(passcode) != CHIP_NO_ERROR) {
            passcode = 0; /* test provider may not expose the passcode */
        }
        ESP_LOGI(TAG, "onboarding: discriminator=%u passcode=%u  (test ids: VID=0xFFF1 PID=0x8000)",
                 (unsigned)disc, (unsigned)passcode);
    }

    char manual[16] = {0}, qr[128] = {0};
    if (lb_matter_setup_code(manual, sizeof(manual), qr, sizeof(qr)) == ESP_OK) {
        ESP_LOGI(TAG, "setup code: %s   (QR payload length %u)", manual, (unsigned)strlen(qr));
    }
}

bool lb_matter_commissioned(void)
{
    return chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
}

/* Push the current local target up to the Matter data model.
 *
 * The lamp has two front ends - this web console and any Matter controller - so
 * whichever one is written must be visible from the other. Matter -> device
 * already happens in attribute_cb(); this is the reverse direction, used after a
 * web request or a sun-curve update. Only differing attributes are written, so a
 * no-op call costs nothing and cannot ping-pong with the callback. */
esp_err_t lb_matter_sync_target(void)
{
    if (s_endpoint_id == 0 || s_syncing) {
        return ESP_ERR_INVALID_STATE;
    }
    {
        /* While diagnostic drive owns the outputs the controller must keep seeing
         * the physical state, not the stored target (sun curve, web and CLI all
         * call this after lightbar_out_set). */
        lb_diag_drive_t d;
        lightbar_out_get_diag(&d);
        if (d.on) {
            lb_matter_report_diag();
            return ESP_OK;
        }
    }
    lb_target_t t = { 0 };
    lightbar_out_get(&t);
    s_syncing = true;

    esp_matter_attr_val_t val;
    if ((bool)t.on != s_on) {
        val = esp_matter_bool((bool)t.on);
        attribute::update(s_endpoint_id, CLUSTER_ON_OFF, ATTR_ON_OFF, &val);
    }
    if (t.level != 0 && t.level != s_level) {
        val = esp_matter_uint8(t.level);
        attribute::update(s_endpoint_id, CLUSTER_LEVEL_CONTROL, ATTR_CURRENT_LEVEL, &val);
    }
    if (t.mireds != s_mireds) {
        val = esp_matter_uint16(t.mireds);
        attribute::update(s_endpoint_id, CLUSTER_COLOR_CONTROL, ATTR_COLOR_TEMP_MIREDS, &val);
    }

    s_syncing = false;
    return ESP_OK;
}

/* Diagnostic drive changed (web / CLI / Mode Select). Publish what the lamp is
 * physically doing so the controller's light entity does not lie, and keep the
 * Mode Select CurrentMode in step. While diagnostic drive is on the real CCT
 * target is untouched; when it goes off the real target is republished. */
extern "C" void lb_matter_report_diag(void)
{
    if (s_endpoint_id == 0 || s_syncing) {
        return;
    }
    lb_diag_drive_t d;
    lightbar_out_get_diag(&d);
    s_syncing = true;
    esp_matter_attr_val_t val = esp_matter_uint8(d.on ? LB_MODE_DIAG : LB_MODE_NORMAL);
    attribute::update(s_endpoint_id, CLUSTER_MODE_SELECT, ATTR_CURRENT_MODE, &val);
    if (d.on) {
        bool on = (d.warm_pct > 0 || d.cold_pct > 0);
        unsigned mx = d.warm_pct > d.cold_pct ? d.warm_pct : d.cold_pct;
        uint8_t level = on ? (uint8_t)((mx * 254u + 50u) / 100u) : s_level;
        if (on && level == 0) level = 1;
        uint16_t mireds = s_mireds;
        if (on) { /* warm only -> 2700 K, cold only -> 5000 K, else by ratio */
            mireds = (uint16_t)(LB_CT_COLD_MIREDS + ((LB_CT_WARM_MIREDS - LB_CT_COLD_MIREDS) * (unsigned)d.warm_pct) / (d.warm_pct + d.cold_pct));
        }
        val = esp_matter_bool(on);
        attribute::update(s_endpoint_id, CLUSTER_ON_OFF, ATTR_ON_OFF, &val);
        val = esp_matter_uint8(level);
        attribute::update(s_endpoint_id, CLUSTER_LEVEL_CONTROL, ATTR_CURRENT_LEVEL, &val);
        val = esp_matter_uint16(mireds);
        attribute::update(s_endpoint_id, CLUSTER_COLOR_CONTROL, ATTR_COLOR_TEMP_MIREDS, &val);
        s_syncing = false;
        /* shadows deliberately NOT updated: s_on/s_level/s_mireds stay = real target */
    } else {
        s_syncing = false;
        lb_matter_sync_target();
    }
}

/* CHIP APIs must run on the Matter task. Calling ConnectWiFiNetwork() straight * from the HTTP task trips "Chip stack locking error" and reboots the chip, which
 * is why provisioning used to die halfway with the browser left waiting on
 * "sending to device". Both helpers below only queue the work. */
enum wifi_action_t {
    WIFI_ACT_CONNECT,
    WIFI_ACT_FORGET,
    WIFI_ACT_PAUSE,
    WIFI_ACT_FACTORY_RESET,
};

struct wifi_job_t {
    char ssid[33];
    char pass[65];
    wifi_action_t action;
};

static void wifi_job_run(intptr_t arg)
{
    wifi_job_t *j = reinterpret_cast<wifi_job_t *>(arg);
    if (j == nullptr) {
        return;
    }
    switch (j->action) {
    case WIFI_ACT_FORGET: {
        CHIP_ERROR err = chip::DeviceLayer::Internal::ESP32Utils::ClearWiFiStationProvision();
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "ClearWiFiStationProvision: %" CHIP_ERROR_FORMAT, err.Format());
        } else {
            ESP_LOGW(TAG, "wifi provision cleared");
        }
        break;
    }
    case WIFI_ACT_FACTORY_RESET: {
        /* Erases the esp_matter data-model namespace, then CHIP's own factory
         * reset: fabrics + NOCs + Wi-Fi station provision gone, chip restarts. */
        ESP_LOGW(TAG, "FACTORY RESET: erasing fabrics + Wi-Fi provision, restarting");
        evlog("matter", "factory_reset executing");
        esp_err_t e = esp_matter::factory_reset();
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "esp_matter::factory_reset: %s", esp_err_to_name(e));
        }
        break;
    }
    case WIFI_ACT_PAUSE: {
        /* A failed join leaves ConnectivityManager retrying the network every
         * 100 ms forever, which floods the log, starves the HTTP server and makes
         * every scan fail with "STA is connecting". Park the station until the
         * user submits new credentials. */
        CHIP_ERROR err = chip::DeviceLayer::ConnectivityMgr().SetWiFiStationMode(
            chip::DeviceLayer::ConnectivityManager::kWiFiStationMode_Disabled);
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "pause station: %" CHIP_ERROR_FORMAT, err.Format());
        } else {
            ESP_LOGW(TAG, "wifi station parked (stopped retrying)");
        }
        break;
    }
    default: {
        auto &drv = chip::DeviceLayer::NetworkCommissioning::ESPWiFiDriver::GetInstance();
        CHIP_ERROR err = drv.ConnectWiFiNetwork(j->ssid, static_cast<uint8_t>(strlen(j->ssid)), j->pass,
                                                static_cast<uint8_t>(strlen(j->pass)));
        if (err != CHIP_NO_ERROR) {
            ESP_LOGE(TAG, "ConnectWiFiNetwork: %" CHIP_ERROR_FORMAT, err.Format());
        } else {
            ESP_LOGW(TAG, "wifi join started for \"%s\" (password %s)", j->ssid,
                     j->pass[0] ? "supplied" : "EMPTY");
        }
        break;
    }
    }
    free(j);
}

static esp_err_t schedule_wifi_job(wifi_job_t *j)
{
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(wifi_job_run, reinterpret_cast<intptr_t>(j));
    if (err != CHIP_NO_ERROR) {
        free(j);
        ESP_LOGE(TAG, "ScheduleWork failed: %" CHIP_ERROR_FORMAT, err.Format());
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t lb_matter_wifi_station_pause(void)
{
    wifi_job_t *j = reinterpret_cast<wifi_job_t *>(calloc(1, sizeof(wifi_job_t)));
    if (j == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    j->action = WIFI_ACT_PAUSE;
    return schedule_wifi_job(j);
}

esp_err_t lb_matter_factory_reset(void)
{
    wifi_job_t *j = reinterpret_cast<wifi_job_t *>(calloc(1, sizeof(wifi_job_t)));
    if (j == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    j->action = WIFI_ACT_FACTORY_RESET;
    return schedule_wifi_job(j);
}

esp_err_t lb_matter_wifi_forget(void)
{
    wifi_job_t *j = reinterpret_cast<wifi_job_t *>(calloc(1, sizeof(wifi_job_t)));
    if (j == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    j->action = WIFI_ACT_FORGET;
    return schedule_wifi_job(j);
}

esp_err_t lb_matter_wifi_connect(const char *ssid, const char *password)
{
    if (ssid == nullptr || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    wifi_job_t *j = reinterpret_cast<wifi_job_t *>(calloc(1, sizeof(wifi_job_t)));
    if (j == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    j->action = WIFI_ACT_CONNECT;
    snprintf(j->ssid, sizeof(j->ssid), "%s", ssid);
    snprintf(j->pass, sizeof(j->pass), "%s", (password != nullptr) ? password : "");
    return schedule_wifi_job(j);
}

esp_err_t lb_matter_setup_code(char *manual, size_t manual_len, char *qr, size_t qr_len)
{
    chip::DeviceLayer::CommissionableDataProvider *cdp = chip::DeviceLayer::GetCommissionableDataProvider();
    if (cdp == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t disc = 0;
    uint32_t passcode = 0;
    if (cdp->GetSetupDiscriminator(disc) != CHIP_NO_ERROR) {
        return ESP_FAIL;
    }
    if (cdp->GetSetupPasscode(passcode) != CHIP_NO_ERROR || passcode == 0) {
        return ESP_FAIL;
    }

    chip::SetupPayload payload;
    payload.version = 0;
    payload.vendorID = 0xFFF1;
    payload.productID = 0x8000;
    payload.commissioningFlow = chip::CommissioningFlow::kStandard;
    payload.rendezvousInformation.SetValue(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
    payload.discriminator.SetLongValue(disc);
    payload.setUpPINCode = passcode;

    if (manual != nullptr && manual_len > 0) {
        std::string out;
        chip::ManualSetupPayloadGenerator gen(payload);
        if (gen.payloadDecimalStringRepresentation(out) != CHIP_NO_ERROR) {
            return ESP_FAIL;
        }
        snprintf(manual, manual_len, "%s", out.c_str());
    }
    if (qr != nullptr && qr_len > 0) {
        std::string out;
        chip::QRCodeSetupPayloadGenerator gen(payload);
        if (gen.payloadBase38Representation(out) != CHIP_NO_ERROR) {
            return ESP_FAIL;
        }
        snprintf(qr, qr_len, "%s", out.c_str());
    }
    return ESP_OK;
}
