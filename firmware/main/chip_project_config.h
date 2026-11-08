/* chip_project_config.h - Matter Basic Information strings for this device.
 *
 * Pulled in by connectedhomeip through CHIP_PROJECT_CONFIG_INCLUDE
 * (CONFIG_CHIP_PROJECT_CONFIG in sdkconfig points here), BEFORE
 * platform/CHIPDeviceConfig.h applies its "TEST_VENDOR"/"TEST_PRODUCT" defaults.
 * This is what Home Assistant shows on the device page.
 *
 * VendorID/ProductID stay at the test values 0xFFF1/0x8000: real IDs need a CSA
 * membership and a certified DAC; the test IDs work in HA developer mode. The
 * software version string comes from the app descriptor (version.txt).
 * Max lengths (BasicInformation cluster): VendorName 32, ProductName 32,
 * HardwareVersionString 64. */
#pragma once

#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME                     "Yanzhanc"
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME                    "LightBar with SunLike LED"
#define CHIP_DEVICE_CONFIG_DEFAULT_DEVICE_HARDWARE_VERSION_STRING "Engineering Sample"
