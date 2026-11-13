/* LightBar device console.
 *
 * One page, four tabs: overview (state + light control), update (OTA), wifi
 * (provisioning + Matter code), advanced (runtime parameters, driver registers,
 * fault history - the debug view).
 *
 * Everything the page needs is a plain GET returning JSON, so the same endpoints
 * can be polled by an engineer over a tunnel.
 */
(function () {
  'use strict';
  var $ = function (id) { return document.getElementById(id); };

  /* fetch with a deadline: if the device reboots or drops off the AP mid-request
   * the promise would otherwise stay pending forever and the UI would look stuck. */
  function fetchT(url, opts, ms) {
    opts = opts || {};
    if (!window.AbortController) return fetch(url, opts);
    var ctl = new AbortController();
    var id = setTimeout(function () { ctl.abort(); }, ms || 12000);
    opts.signal = ctl.signal;
    return fetch(url, opts).then(
      function (r) { clearTimeout(id); return r; },
      function (e) { clearTimeout(id); throw e; }
    );
  }

  /* ---------------------------------------------------------------- tabs */
  var TITLES = {
    overview: ['运行概览', '当前设备状态'],
    update: ['软件更新', '上传固件，设备校验后写入备用分区并重启。'],
    wifi: ['网络设置', '把设备接入你的 Wi-Fi,然后添加到 Home Assistant。'],
    help: ['使用说明', '状态灯颜色的含义,以及几种常见灯光现象的解释。'],
    advanced: ['高级 / 调试', '实时运行参数、调音、驱动寄存器与故障历史。']
  };
  var currentTab = 'overview';
  function showTab(name) {
    currentTab = name;
    Array.prototype.forEach.call(document.querySelectorAll('.tab'), function (t) {
      t.className = 'tab' + (t.getAttribute('data-tab') === name ? ' active' : '');
    });
    Array.prototype.forEach.call(document.querySelectorAll('.tab-page'), function (p) {
      p.hidden = p.id !== 'page-' + name;
    });
    var t = TITLES[name];
    if (t) { $('hero-title').textContent = t[0]; $('hero-sub').textContent = t[1]; }
    if (name === 'advanced') { loadDiag(); loadLog(); }
    if (name === 'wifi') { scan(); pollProv(); }
    if (location.hash !== '#' + name) {
      try { history.replaceState(null, '', '#' + name); } catch (e) { /* file:// preview */ }
    }
  }
  Array.prototype.forEach.call(document.querySelectorAll('.tab'), function (t) {
    t.addEventListener('click', function () { showTab(t.getAttribute('data-tab')); });
  });

  /* -------------------------------------------------------------- status */
  var STATE_TEXT = {
    ARM_OK: '运行中', DISARMED: '已停止输出', BLOCKED_CRCERR: '启动故障锁定',
    NOCHIPS: '驱动未响应', BOOT: '启动中'
  };
  var status = null, suppressControl = false;

  function kToMireds(k) { return Math.round(1000000 / k); }
  /* Mireds are the value that actually goes to the driver and to Matter, and the
   * physical range is 370..200 mireds (= 2702.7 K .. 5000 K). Rounding straight
   * to whole kelvin would show "2703K" at the warm end, so display in 10 K steps:
   * 370 -> 2700K, 200 -> 5000K. */
  function miredsToK(m) { return Math.round(1000000 / m / 10) * 10; }
  /* Device/Matter level is 1..254; the page shows it as 0..100 %. */
  function levelPct(l) { return Math.round(l / 254 * 100) + '%'; }
  function fmtUptime(s) {
    var d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60);
    if (d) return d + ' 天 ' + h + ' 小时';
    if (h) return h + ' 小时 ' + m + ' 分';
    return m + ' 分';
  }

  function renderStatus(j, first) {
    status = j;
    $('version').textContent = j.version;
    var text = STATE_TEXT[j.state] || j.state;
    if (j.overtemp) text += ' · 板温过高';
    if (j.driver === 'stuck') text += ' · 一路驱动故障';
    $('mode').textContent = text;

    var cls = 'idle';
    if (j.state === 'ARM_OK') cls = 'ok';
    else if (j.state === 'DISARMED' || j.state === 'NOCHIPS' || j.driver === 'stuck') cls = 'bad';
    else if (j.state === 'BLOCKED_CRCERR') cls = 'warn';
    $('dot').className = 'status-dot ' + cls;

    $('net-badge').textContent = j.ip ? ('已联网 ' + j.ip) : (j.ap_active ? '配置模式' : '未联网');
    var apiIp = j.ip || (j.ap_active ? '192.168.4.1' : location.hostname);
    $('api-base').textContent = 'http://' + apiIp + '/';
    Array.prototype.forEach.call(document.querySelectorAll('.api-ip'), function (el) { el.textContent = apiIp; });
    $('temp').textContent = j.temp_valid ? j.temp.toFixed(1) + ' °C' : '不可用';
    $('temp-state').textContent = j.overtemp ? '超温保护中' : '正常';
    $('chips').textContent = j.chips_ack === 2 ? '两颗正常' : (j.chips_ack === 1 ? '仅一颗响应' : '均未响应');
    $('gate').textContent = j.gate ? '打开（允许输出）' : '关闭';
    $('commissioned').textContent = j.commissioned ? '已加入 fabric' : '未配网（等待添加）';

    /* The supply can refuse more current than it can deliver; the firmware then
     * walks the brightness ceiling down and remembers it. Say so, otherwise the
     * lamp just looks dimmer than the slider claims. */
    var capEl = $('cap-warn');
    if (j.level_cap && j.level_cap < 254) {
      capEl.hidden = false;
      capEl.textContent = '亮度已被限制在 ' + Math.round(j.level_cap / 254 * 100) +
        '%。这是在「高级」里设置的亮度上限,可随时改回 100%。';
      $('level').max = j.level_cap;
    } else {
      capEl.hidden = true;
      $('level').max = 254;
    }
    $('light-state').textContent = j.light.on ? ('开 · ' + miredsToK(j.light.mireds) + 'K') : '关';

    /* Per-string health from LEDx_FAULT. The driver disables a faulted string and
     * keeps the bit until a power cycle, so this is also the "reconnect" story. */
    if (j.strings) {
      var KIND = function (k) { return (k & 4) ? '对地短路' : (k & 2) ? '短路' : '开路'; };
      var faulty = [];
      ['warm', 'cold'].forEach(function (ch) {
        var s = j.strings[ch], el = $(ch === 'warm' ? 'str-warm' : 'str-cold'), html = '';
        /* Four physical outputs per driver, plain text: OUT1 正常 / 开路 / 短路 /
         * 对地短路, outputs beyond the configured string count show 未用. */
        var chip = (j.chips && j.chips[ch === 'warm' ? 0 : 1]) || null;
        var chipStuck = chip && chip.stuck, chipOff = chip && !chip.want;
        for (var i = 0; i < 4; i++) {
          var used = i < j.strings.n, bad = used && ((s.fault >> i) & 1);
          var txt = !used ? '未用' : (bad ? KIND(s.kind) : (chipStuck ? '驱动故障' : (chipOff ? '关' : '正常')));
          html += '<span class="' + (bad || (used && chipStuck) ? 'bad' : (used && !chipOff ? '' : 'na')) + '">OUT' + (i + 1) + ' ' + txt + '</span>';
          if (bad) faulty.push((ch === 'warm' ? '2700K OUT' : '5000K OUT') + (i + 1) + ' ' + KIND(s.kind));
        }
        el.innerHTML = html;
      });
      var sw = $('string-warn');
      if (faulty.length) {
        sw.hidden = false;
        $('string-warn-text').textContent = faulty.join('、') + ',该串已被驱动关闭。' +
          (j.reprobe ? '接好后点「重新检测灯串」即可恢复。' : '接好后需要把灯具电源(USB-C)拔掉再插上才会重新点亮(本板 EN 硬接,软件无法复位驱动)。');
        $('reprobe').hidden = false;
      } else {
        sw.hidden = true;
      }
    }
    /* A latched driver looks exactly like "nothing happens" from the UI, so say it
     * out loud: the outputs are forced off and only a VBUS power cycle clears it. */
    var warn = $('driver-warn');
    if (j.driver === 'stuck') {
      var which = [];
      if (j.chips) { if (j.chips[0].stuck) which.push('2700K'); if (j.chips[1].stuck) which.push('5000K'); }
      warn.hidden = false;
      warn.textContent = which.join(' / ') + ' 驱动被输入过流保护锁在恢复循环里(FSM 不断在 FAULT_RECOVERY 和启动之间往返),该路不亮。' +
        '任何亮度命令都无法让它恢复:请把灯具电源(USB-C)拔掉再插上。本板 EN 硬接 3.3V,软件无法复位驱动。';
    } else if (j.driver === 'latched') {
      warn.hidden = false;
      warn.textContent = '驱动已进入故障锁存（ALL_LED_FAULT），输出被强制关闭，调亮度和色温都不会有效果。' +
        '请把灯带的电源（VBUS/PD）完全断开再重新上电即可清除；EN 在本板硬接 3.3V，软件无法复位。';
    } else if (j.driver === 'recovering') {
      warn.hidden = false;
      warn.textContent = '驱动正在故障恢复中（FAULT_RECOVERY），输出会暂时关闭，稍等几秒。';
    } else {
      warn.hidden = true;
    }

    /* Fresh reboot: Matter controllers (Home Assistant) need to rebuild their
     * session/subscription before they can control the lamp again. Say so, or
     * the lamp looks dead for a minute or two. */
    var bw = $('boot-warn');
    if (typeof j.uptime === 'number' && j.uptime < 180) {
      bw.hidden = false;
      bw.textContent = '设备 ' + Math.round(j.uptime) + ' 秒前重启。' +
        (j.commissioned ? 'Home Assistant / Matter 控制器正在重新建立连接(通常 30 秒到 3 分钟),期间请用本页控制。' : '');
    } else {
      bw.hidden = true;
    }

    /* Keep the controls in sync with the device when the user is not touching
     * them (e.g. HA changed the light, or the page was open across a reboot). */
    if (!first && Date.now() - lastUserInput > 4000) {
      suppressControl = true;
      if (+$('level').value !== j.light.level) { $('level').value = j.light.level; $('level-val').textContent = levelPct(j.light.level); }
      if (!autoOn && !draggingCt && +$('mireds').value !== j.light.mireds) { $('mireds').value = j.light.mireds; $('mireds-val').textContent = miredsToK(j.light.mireds) + 'K'; }
      setOnSeg(j.light.on ? '1' : '0');
      suppressControl = false;
    }

    if (first) {
      suppressControl = true;
      $('level').value = j.light.level;
      $('level-val').textContent = levelPct(j.light.level);
      $('mireds').value = j.light.mireds;
      $('mireds-val').textContent = miredsToK(j.light.mireds) + 'K';
      setOnSeg(j.light.on ? '1' : '0');
      suppressControl = false;
    }

    /* Sun-following state: while it is on, the colour slider mirrors what the
     * device computed instead of the stored target. */
    autoOn = !!(j.auto && j.auto.on);
    $('auto-sw').checked = autoOn;
    if (autoOn && j.auto.mireds && !draggingCt) {
      $('mireds').value = j.auto.mireds;
      $('mireds-val').textContent = miredsToK(j.auto.mireds) + 'K';
      $('auto-hint').textContent = '太阳高度 ' + j.auto.elev.toFixed(1) + '° → ' + miredsToK(j.auto.mireds) + 'K。手动拖动色温条会退出自适应。';
    } else {
      $('auto-hint').textContent = autoOn ? '等待日照数据。手动拖动色温条会退出自适应。' : '跟随太阳高度自动调整色温。手动拖动下面的色温条会退出自适应。';
    }

    if (typeof j.current_code === 'number' && Date.now() - lastUserInput > 4000) { renderCur(j.current_code >> 4); $('cur-state').textContent = '已保存'; }
    if (j.rev && !capsApplied) { capsApplied = true; applyRev(j); }
    if (j.hw) { renderHw(j.hw); }
    if (j.diag) { ddRender(j.diag); }
    if (typeof j.led_brightness === 'number' && !ledTouched) {
      $('led-bri').value = j.led_brightness;
      $('led-bri-val').textContent = Math.round(j.led_brightness / 255 * 100) + '%';
    }
    $('device-note').textContent = j.ota_busy ? '设备正在写入固件'
      : (j.overtemp ? '板温过高，暂不允许更新' : '可以更新');
    syncButtons();
  }

  function pollStatus(first) {
    fetch('api/status', { cache: 'no-store' }).then(function (r) { return r.json(); })
      .then(function (j) { renderStatus(j, first); }).catch(function () {});
  }

  /* --------------------------------------------------------- light control */
  var autoOn = false, draggingCt = false, lastUserInput = 0, capsApplied = false;

  function setOnSeg(v) {
    $('on-sw').checked = (v === '1');
  }
  /* withCt=false leaves the colour untouched on the device, which is what keeps
   * dimming from cancelling sun-following. */
  var lightTimer = null;
  function pushLight(on, level, mireds, withCt) {
    if (suppressControl) return;
    lastUserInput = Date.now();
    /* Touching a slider means "I want light": turn the lamp on, like every
     * lamp app does. Only the explicit 关 button turns it off. */
    if (on && status) { status.light.on = 1; }
    setOnSeg(on ? '1' : '0');
    if (lightTimer) clearTimeout(lightTimer);
    lightTimer = setTimeout(function () {
      var body = 'on=' + on + '&level=' + level + (withCt ? '&mireds=' + mireds : '');
      fetch('api/light', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: body })
        .then(function (r) { return r.json(); })
        .then(function (j) {
          $('light-note').textContent = j.applied ? '已下发到驱动。' :
            '目标已保存，但被安全门禁拦下（' + (j.reason || 'gate closed') + '）。';
          if (j.auto === 0 && autoOn) { autoOn = false; $('auto-sw').checked = false; }
        }).catch(function () { $('light-note').textContent = '下发失败。'; });
    }, 180);
  }

  /* Sun-following: turning it on recomputes and applies immediately. */
  function setAuto(on) {
    fetch('api/auto', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'on=' + (on ? 1 : 0)
    }).then(function (r) { return r.json(); }).then(function (j) {
      autoOn = !!j.auto;
      $('auto-sw').checked = autoOn;
      if (autoOn && j.mireds) {
        $('mireds').value = j.mireds;
        $('mireds-val').textContent = miredsToK(j.mireds) + 'K';
      }
      $('light-note').textContent = autoOn
        ? '自适应已开启：按太阳高度自动调整色温（太阳高度 ' + j.elev.toFixed(1) + '°）。'
        : '自适应已关闭，色温由你手动控制。';
      pollStatus(false);
    }).catch(function () { $('light-note').textContent = '切换自适应失败。'; });
  }

  $('on-sw').addEventListener('change', function () {
    pushLight(this.checked ? 1 : 0, +$('level').value, +$('mireds').value, !autoOn);
  });

  $('auto-sw').addEventListener('change', function () { setAuto(this.checked); });

  $('level').addEventListener('input', function () {
    $('level-val').textContent = levelPct(+this.value);
    pushLight(1, +this.value, +$('mireds').value, !autoOn);
  });

  var elCt = $('mireds');
  ['pointerdown', 'touchstart'].forEach(function (ev) { elCt.addEventListener(ev, function () { draggingCt = true; }); });
  ['pointerup', 'touchend', 'pointercancel', 'blur'].forEach(function (ev) {
    elCt.addEventListener(ev, function () { setTimeout(function () { draggingCt = false; }, 400); });
  });
  elCt.addEventListener('input', function () {
    $('mireds-val').textContent = miredsToK(+this.value) + 'K';
    /* Manual colour beats the sun curve - same rule as a Matter controller
     * writing ColorTemperatureMireds. */
    if (autoOn) {
      autoOn = false;
      $('auto-sw').checked = false;
      fetch('api/auto', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'on=0' }).catch(function () {});
    }
    pushLight(1, +$('level').value, +this.value, true);
  });

  /* ----------------------------------------------------------------- OTA */
  var elFile = $('file'), elUpload = $('upload'), elCancel = $('cancel');
  var elPct = $('percent'), elBar = $('progress'), elStatus = $('status'), elDetails = $('details');
  var baseVersion = null, phase = 'idle', xhr = null, rebootedAt = 0;

  function setStep(i) {
    ['step-upload', 'step-verify', 'step-write', 'step-reboot'].forEach(function (id, k) {
      var el = $(id); if (el) { el.className = (k === i) ? 'active' : ''; }
    });
  }
  function paint(pct, text, failed) {
    elBar.value = pct; elPct.textContent = pct + '%';
    elStatus.textContent = text; elStatus.className = failed ? 'error' : '';
    elBar.className = failed ? 'failed' : '';
  }
  function syncButtons() {
    var busy = status && status.ota_busy, blocked = status && status.overtemp;
    var hasFile = elFile.files && elFile.files.length > 0;
    elUpload.disabled = !(hasFile && !busy && !blocked && (phase === 'idle' || phase === 'failed' || phase === 'done'));
    elCancel.disabled = (phase !== 'upload');
  }
  function fail(t, d) { phase = 'failed'; paint(100, t, true); elDetails.textContent = d; syncButtons(); }

  function upload() {
    var f = elFile.files && elFile.files[0];
    if (!f) return;
    baseVersion = status ? status.version : null;
    phase = 'upload'; setStep(0);
    paint(0, '准备上传 ' + (f.size / 1024 / 1024).toFixed(2) + ' MB…', false);
    elDetails.textContent = '上传过程中请不要断电。';
    xhr = new XMLHttpRequest();
    xhr.open('POST', 'update', true);
    xhr.setRequestHeader('Content-Type', 'application/octet-stream');
    xhr.upload.onprogress = function (e) {
      if (!e.lengthComputable) return;
      var pct = Math.min(78, Math.round((e.loaded / e.total) * 78));
      setStep(pct < 26 ? 0 : (pct < 52 ? 1 : 2));
      paint(pct, '正在上传… ' + Math.round((e.loaded / e.total) * 100) + '%', false);
    };
    xhr.onload = function () {
      if (xhr.status === 200 || xhr.status === 202) {
        phase = 'reboot'; rebootedAt = Date.now(); setStep(3);
        paint(92, '上传完成，设备正在校验并切换分区…', false);
        elDetails.textContent = '校验通过后设备会自动重启；若新固件无法启动，引导程序会回滚到 ' + baseVersion + '。';
        syncButtons();
      } else {
        fail('设备拒绝了这次更新（HTTP ' + xhr.status + '）。', xhr.responseText || '可能板温过高或已有更新在进行。');
      }
    };
    xhr.onerror = function () { fail('上传中断。', '连接被重置，重试即可，分区未被激活。'); };
    xhr.onabort = function () { phase = 'idle'; setStep(-1); paint(0, '已取消。', false); syncButtons(); };
    xhr.send(f);
  }
  elFile.addEventListener('change', function () {
    var f = elFile.files && elFile.files[0];
    $('file-label').textContent = f ? f.name + ' · ' + (f.size / 1024 / 1024).toFixed(2) + ' MB'
      : '从项目 Releases 页面下载对应的 .bin';
    if (phase === 'failed' || phase === 'done') { phase = 'idle'; setStep(-1); paint(0, '已就绪。', false); }
    syncButtons();
  });
  elUpload.addEventListener('click', upload);
  elCancel.addEventListener('click', function () { if (xhr) xhr.abort(); });

  /* --------------------------------------------------------------- Wi-Fi */
  var elNets = $('nets'), elPass = $('pass'), elJoin = $('join');
  var selected = null, scanTimer = null, lastProv = null;

  function bars(rssi) {
    var n = rssi > -55 ? 4 : rssi > -67 ? 3 : rssi > -78 ? 2 : 1, s = '';
    for (var i = 0; i < 4; i++) s += '<i class="' + (i < n ? 'on' : '') + '"></i>';
    return '<span class="bars">' + s + '</span>';
  }
  function renderNets(j) {
    if (j.scanning) {
      elNets.innerHTML = '<div class="net-placeholder">正在扫描附近网络…</div>';
      $('scan-state').textContent = '正在扫描'; return;
    }
    if (!j.nets || !j.nets.length) {
      var why = j.error && j.error !== 'ESP_OK' ? ('（' + j.error + '）') : '';
      elNets.innerHTML = '<div class="net-placeholder">没有扫描到网络' + why +
        '。确认路由器开了 2.4 GHz（本机不支持 5 GHz），或点「重新扫描」。</div>';
      $('scan-state').textContent = '未找到网络';
      elJoin.disabled = (selected === null);
      return;
    }
    $('scan-state').textContent = '找到 ' + j.nets.length + ' 个网络';
    var html = '';
    for (var i = 0; i < j.nets.length; i++) {
      var n = j.nets[i];
      var esc = n.ssid.replace(/[&<>"]/g, function (c) {
        return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c];
      });
      html += '<button class="net' + (selected === n.ssid ? ' sel' : '') + '" data-ssid="' + esc + '">' +
        '<span class="net-name">' + esc + (n.secure ? '<em class="lock">🔒</em>' : '') + '</span>' + bars(n.rssi) + '</button>';
    }
    elNets.innerHTML = html;
    Array.prototype.forEach.call(elNets.querySelectorAll('.net'), function (b) {
      b.addEventListener('click', function () {
        selected = b.getAttribute('data-ssid');
        Array.prototype.forEach.call(elNets.querySelectorAll('.net'), function (x) { x.className = 'net'; });
        b.className = 'net sel';
        elJoin.disabled = false;
        $('join-note').textContent = '已选择“' + selected + '”';
      });
    });
  }
  function pollScan() {
    fetch('api/scan', { cache: 'no-store' }).then(function (r) { return r.json(); }).then(function (j) {
      renderNets(j);
      if (j.scanning) { scanTimer = setTimeout(pollScan, 1200); }
    }).catch(function () { $('scan-state').textContent = '读取失败'; });
  }
  function scan() {
    if (scanTimer) clearTimeout(scanTimer);
    selected = null; elJoin.disabled = true;
    fetch('api/scan?start=1', { cache: 'no-store' }).catch(function () {});
    pollScan();
  }
  function pollProv() {
    fetch('api/prov', { cache: 'no-store' }).then(function (r) { return r.json(); }).then(function (j) {
      lastProv = j;
      $('ap-ssid').textContent = j.ap_active ? j.ap_ssid : (j.ip || '已联网');
      $('ap-state').textContent = j.ap_active ? '热点开启' : (j.ip ? '已联网' : '空闲');
      $('ap-hint').textContent = '热点密码 ' + (j.ap_pass || '设备 MAC 大写、无冒号') + (j.ap_active ? '，连上后回到本页' : (j.ip ? ' · IP ' + j.ip : ''));
      if (j.manual_code) { $('manual-code').textContent = j.manual_code; }
      $('code-note').textContent = j.commissioned ? '设备已加入 Matter fabric。'
        : '当前未加入任何 fabric，配对码输入后即可添加。';
      var st = j.state;
      if (st === 'connecting') { $('prov-text').textContent = '正在加入网络，约需 10 秒…'; $('prov-bar').value = 55; }
      else if (st === 'connected') { $('prov-text').textContent = '连接成功，设备地址 ' + (j.ip || '—'); $('prov-bar').value = 100; }
      else if (st === 'failed') { $('prov-text').textContent = '连接失败：密码错误、路由器只开了 5 GHz、或信号太弱。改好密码再试就行，设备热点和这个页面会一直开着。'; $('prov-bar').value = 100; $('prov-bar').className = 'failed'; elJoin.disabled = (selected === null); $('join-note').textContent = selected ? ('可以重试：' + selected) : ''; }
      else if (st === 'ap') { $('prov-text').textContent = '设备热点已开启，选择网络并输入密码。'; $('prov-bar').value = 15; }
      else { $('prov-text').textContent = j.ip ? '已联网：' + j.ip : '等待操作。'; $('prov-bar').value = j.ip ? 100 : 0; }
    }).catch(function () {});
  }
  function submitJoin(ssid) {
    if (!ssid) return;
    elJoin.disabled = true;
    $('join-manual').disabled = true;
    $('join-note').textContent = '正在发送给设备…';
    fetchT('api/prov', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: 'ssid=' + encodeURIComponent(ssid) + '&pass=' + encodeURIComponent(elPass.value)
    }, 15000).then(function (r) {
      if (!r.ok) throw new Error('HTTP ' + r.status);
      $('join-note').textContent = '已提交，等待设备连接…';
      pollProv();
    }).catch(function (e) {
      $('join-note').textContent = '设备没有响应（' + e.name + '）。如果手机掉线了，重新连回设备热点（名字以 LightBar-ES- 开头，密码为设备 12 位大写 MAC）再打开本页。';
      elJoin.disabled = false;
      $('join-manual').disabled = false;
    });
  }
  elJoin.addEventListener('click', function () { submitJoin(selected); });
  $('join-manual').addEventListener('click', function () {
    var v = $('ssid-manual').value.trim();
    if (!v) { $('join-note').textContent = '先输入 Wi-Fi 名称。'; return; }
    selected = v;
    submitJoin(v);
  });
  $('rescan').addEventListener('click', scan);

  /* ---------------------------------------------------------- advanced/diag */
  var REGNAME = {
    '00': 'BRT 亮度', '02': 'LED 电流', '0E': 'SUP 故障', '10': 'BOOST 故障', '12': 'LED 故障',
    '14': 'FSM 状态', '16': 'PWM 输入', '18': 'PWM 输出', '1A': 'DAC 实际码', '1C': '自适应 boost', '1E': '自动检测'
  };
  function loadDiag() {
    fetch('api/diag', { cache: 'no-store' }).then(function (r) { return r.json(); }).then(function (j) {
      var kv = '';
      function row(k, v) { kv += '<div><dt>' + k + '</dt><dd>' + v + '</dd></div>'; }
      row('固件版本', j.version + ' · ' + j.build);
      row('运行分区', j.running + ' @ ' + j.running_addr);
      row('备用分区', j.next + ' @ ' + j.next_addr);
      row('运行时间', fmtUptime(j.uptime));
      row('内部可用堆', Math.round(j.heap / 1024) + ' KB（最小 ' + Math.round(j.heap_min / 1024) + ' KB）');
      row('最大连续块', Math.round(j.heap_block / 1024) + ' KB');
      row('板载温度', j.temp_valid ? j.temp.toFixed(2) + ' °C' : '不可用');
      row('状态机', j.state + (j.overtemp ? ' · 超温锁存' : ''));
      row('故障计数', j.crit_events + ' 次');
      row('启动次数', j.boots);
      row('输出目标', (j.light.on ? '开' : '关') + ' · 亮度 ' + levelPct(j.light.level) + ' · ' + miredsToK(j.light.mireds) + 'K');
      row('电流码', '0x' + j.current_code.toString(16).toUpperCase() + ' (' + Math.round(j.current_code * 16) + ')');
      row('Wi-Fi', j.ip ? (j.ip + ' · ' + j.rssi + ' dBm') : '未联网');
      row('Matter', j.commissioned ? '已加入 fabric' : '未加入');
      $('diag-kv').innerHTML = kv;

      var tb = '';
      var order = ['00', '02', '0E', '10', '12', '14', '16', '18', '1A', '1C', '1E'];
      for (var i = 0; i < order.length; i++) {
        var r = order[i];
        var w = j.regs.warm && j.regs.warm[r] ? j.regs.warm[r] : '—';
        var c = j.regs.cold && j.regs.cold[r] ? j.regs.cold[r] : '—';
        tb += '<tr><td>' + r + 'h</td><td>' + w + '</td><td>' + c + '</td><td>' + (REGNAME[r] || '') + '</td></tr>';
      }
      $('regs').querySelector('tbody').innerHTML = tb;

      var hb = '';
      if (j.hist && j.hist.length) {
        for (var k = 0; k < j.hist.length; k++) {
          var h = j.hist[k];
          hb += '<tr><td>' + (k + 1) + '</td><td>' + h.reason + '</td><td>' + h.warm + '</td><td>' + h.cold + '</td></tr>';
        }
        $('hist-count').textContent = j.hist.length + ' 条';
      } else {
        hb = '<tr><td colspan="4">暂无记录</td></tr>';
        $('hist-count').textContent = '0 条';
      }
      $('hist').querySelector('tbody').innerHTML = hb;
      $('diag-time').textContent = new Date().toLocaleTimeString();
      $('diag-json').textContent = JSON.stringify(j, null, 1);
      if (j.tune) { renderTune(j.tune); }
    }).catch(function () { $('diag-time').textContent = '读取失败'; });
  }
  $('diag-refresh').addEventListener('click', loadDiag);

  function fmtT(ms) { var s = ms / 1000; return (s >= 60 ? Math.floor(s / 60) + 'm' : '') + (s % 60).toFixed(s >= 60 ? 0 : 1) + 's'; }
  function loadLog() {
    fetch('api/log', { cache: 'no-store' }).then(function (r) { return r.json(); }).then(function (j) {
      var tb = '';
      for (var i = 0; i < j.events.length; i++) {
        var e = j.events[i];
        tb += '<tr><td>' + fmtT(e.t) + '</td><td>' + e.tag + '</td><td>' + e.msg.replace(/</g, '&lt;') + '</td></tr>';
      }
      $('evlog').querySelector('tbody').innerHTML = tb || '<tr><td colspan="3">暂无事件</td></tr>';
    }).catch(function () {});
  }
  $('log-refresh').addEventListener('click', loadLog);

  /* ------------------------------------------------------------- tuning */
  var tuneBusy = false;
  function renderTune(t) {
    if (!t) return;
    tuneBusy = true;
    $('tune-slope').value = String(t.slope);
    $('tune-ssr').value = String(t.ss_range);
    $('tune-ssm').value = String(t.ss_mod);
    $('tune-ssp').checked = !!t.ss_pseudo;
    $('tune-dither').value = String(t.dither);
    $('tune-uc1').textContent = 'UC1 ' + t.uc1;
    $('tune-state').textContent = '已保存';
    tuneBusy = false;
  }
  function pushTune(field, value) {
    if (tuneBusy) return;
    $('tune-state').textContent = '下发中...';
    fetch('api/tune', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: field + '=' + value })
      .then(function (r) { return r.json(); })
      .then(function (j) {
        renderTune(j);
        $('tune-note').textContent = j.ok ? '已写入两颗驱动的 USER_CONFIG1 并保存。'
          : '写入失败(驱动未应答或回读不一致)。';
      }).catch(function () { $('tune-state').textContent = '失败'; });
  }
  function renderCur(code) {
    var sel = $('cur-sel');
    var has = false;
    for (var i = 0; i < sel.options.length; i++) { if (+sel.options[i].value === code) has = true; }
    if (!has) { var o = document.createElement('option'); o.value = code; o.textContent = (160.08 * code * 16 / 4095).toFixed(0) + ' mA(自定义)'; sel.appendChild(o); }
    sel.value = String(code);
  }
  $('cur-sel').addEventListener('change', function () {
    $('cur-state').textContent = '下发中...';
    fetch('api/cur', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'level=' + this.value })
      .then(function (r) { return r.json(); })
      .then(function (j) { renderCur(j.level); $('cur-state').textContent = '已保存'; })
      .catch(function () { $('cur-state').textContent = '失败'; });
  });

  $('tune-slope').addEventListener('change', function () { pushTune('slope', this.value); });
  $('tune-ssr').addEventListener('change', function () { pushTune('ss_range', this.value); });
  $('tune-ssm').addEventListener('change', function () { pushTune('ss_mod', this.value); });
  $('tune-ssp').addEventListener('change', function () { pushTune('ss_pseudo', this.checked ? 1 : 0); });
  $('tune-dither').addEventListener('change', function () { pushTune('dither', this.value); });

  /* Controls that the compiled board revision cannot act on are greyed out;
   * what the chips report about their strap resistors (1Eh) is shown as text. */
  var MODE_NAME = ['相移 PWM', 'Hybrid', '恒流', '直接 PWM', '相移 PWM', 'Hybrid', '恒流', '直接 PWM'];
  function applyRev(j) {
    var isNew = j.rev !== 'PCB3_3';
    var ssOn = !isNew;             /* chip SS only acts when BST_SYNC is tied high (PCB3_3) */
    ['tune-ssr', 'tune-ssm', 'tune-ssp'].forEach(function (id) {
      var el = $(id); el.disabled = !ssOn;
      var row = el.closest('.row'); if (row) row.className = 'row' + (ssOn ? '' : ' off');
    });
    $('ss-cap-note').textContent = ssOn ? '芯片内置,生效中' : '由主控时钟接管,此处不可用';
    $('sync-freq').disabled = !isNew;
    $('sync-freq').closest('.row').className = 'row' + (isNew ? '' : ' off');
    $('sync-cap-note').textContent = isNew ? '主控提供' : ('固件编译版本 ' + j.rev + ':由 BST_FSET 电阻固定');
  }
  function renderHw(h) {
    if (!h) return;
    var line = function (n, c) {
      return n + ':' + (MODE_NAME[c.mode] || c.mode) + ' · ' + c.strings + ' 串 · boost ' + c.boost_khz + ' kHz · PWM ' + c.pwm_hz + ' Hz' + (c.invstring ? ' · 未用 OUT 悬空(INVSTRING)' : '');
    };
    $('sync-hint').textContent = '芯片自检(1Eh,由电阻决定)—— ' + line('2700K', h.warm) + ';' + line('5000K', h.cold) + '。';
  }

  /* --------------------------------------------------- diagnostic drive */
  var ddTimer = null, ddTouched = 0;
  function ddRender(d) {
    if (!d) return;
    $('dd-on').checked = !!d.on;
    ['dd-warm', 'dd-cold', 'dd-both100', 'dd-off'].forEach(function (id) { $(id).disabled = !d.on; });
    if (Date.now() - ddTouched > 3000) {
      $('dd-warm').value = d.warm; $('dd-warm-val').textContent = d.warm + '%';
      $('dd-cold').value = d.cold; $('dd-cold-val').textContent = d.cold + '%';
    }
    $('dd-note').textContent = d.on ? '诊断模式生效中,概览页的灯光控制暂不作用。' : '';
    var lightCard = $('level').closest('.card');
    if (lightCard) lightCard.classList.toggle('diag-locked', !!d.on);
    $('on-sw').disabled = !!d.on;
    $('on-note').textContent = d.on ? '目前在诊断模式' : '';
  }
  function ddPush(body) {
    ddTouched = Date.now();
    if (ddTimer) clearTimeout(ddTimer);
    ddTimer = setTimeout(function () {
      fetch('api/diagdrive', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: body })
        .then(function (r) { return r.json(); })
        .then(function (d) { ddTouched = 0; ddRender(d); if (!d.gate) $('dd-note').textContent = '安全门禁关闭,设定已存但未输出。'; })
        .catch(function () { $('dd-note').textContent = '请求失败'; });
    }, 150);
  }
  $('dd-on').addEventListener('change', function () { ddPush('on=' + (this.checked ? 1 : 0)); });
  $('dd-warm').addEventListener('input', function () { $('dd-warm-val').textContent = this.value + '%'; ddPush('warm=' + this.value); });
  $('dd-cold').addEventListener('input', function () { $('dd-cold-val').textContent = this.value + '%'; ddPush('cold=' + this.value); });
  $('dd-both100').addEventListener('click', function () { $('dd-warm').value = 100; $('dd-cold').value = 100; $('dd-warm-val').textContent = '100%'; $('dd-cold-val').textContent = '100%'; ddPush('warm=100&cold=100'); });
  $('dd-off').addEventListener('click', function () { $('dd-warm').value = 0; $('dd-cold').value = 0; $('dd-warm-val').textContent = '0%'; $('dd-cold-val').textContent = '0%'; ddPush('warm=0&cold=0'); });

  /* --------------------------------------------------- status LED level */  var ledTimer = null, ledTouched = false;
  $('led-bri').addEventListener('input', function () {
    ledTouched = true;
    $('led-bri-val').textContent = Math.round(this.value / 255 * 100) + '%';
    var v = this.value;
    if (ledTimer) clearTimeout(ledTimer);
    ledTimer = setTimeout(function () {
      fetch('api/led', { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: 'brightness=' + v })
        .then(function (r) { return r.json(); })
        .then(function (j) { $('led-note').textContent = '已保存:状态灯亮度 ' + Math.round(j.brightness / 255 * 100) + '%。'; ledTouched = false; })
        .catch(function () { $('led-note').textContent = '保存失败。'; });
    }, 250);
  });

  $('reprobe').addEventListener('click', function () {
    var b = this; b.disabled = true; $('reprobe-note').textContent = '...';
    fetch('api/strings/reprobe', { method: 'POST' }).then(function (r) { return r.json(); }).then(function (j) {
      $('reprobe-note').textContent = j.ok ? '已重启驱动,正在重新检测。' : ('无法执行:' + j.reason);
      b.disabled = false; pollStatus(false);
    }).catch(function () { $('reprobe-note').textContent = '请求失败'; b.disabled = false; });
  });

  /* ------------------------------------------------------- factory reset */
  $('factory').addEventListener('click', function () {
    if (!window.confirm('确定恢复出厂设置?\n\n将清除所有 Matter 配对和 Wi-Fi 凭据,设备重启后进入配网热点模式,需要重新添加到 Home Assistant。')) return;
    var btn = this;
    btn.disabled = true;
    $('factory-note').textContent = '正在发送...';
    var headers = { 'Content-Type': 'application/x-www-form-urlencoded' };
    fetchT('api/factory_reset', { method: 'POST', headers: headers, body: 'confirm=yes' }, 8000)
      .then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        $('factory-note').textContent = '设备正在清除配置并重启。约 15 秒后请在手机 Wi-Fi 列表里连接 LightBar-ES-xxxx 热点（密码 = 设备 MAC 的 12 位大写十六进制、无冒号，也可看串口日志）,再打开 http://192.168.4.1/ 重新配网。';
      }).catch(function (e) {
        $('factory-note').textContent = '失败:' + e.message;
        btn.disabled = false;
      });
  });

  /* ---------------------------------------------------------------- boot */
  var startTab = (location.hash || '#overview').slice(1);
  if (!TITLES[startTab]) { startTab = 'overview'; }
  showTab(startTab);
  pollStatus(true);
  setInterval(function () {
    pollStatus(false);
    if (phase === 'reboot') {
      if (status && baseVersion && status.version !== baseVersion) {
        phase = 'done'; paint(100, '更新完成，设备已在新版本上运行。', false); setStep(-1);
        elDetails.textContent = '版本 ' + baseVersion + ' → ' + status.version;
        syncButtons();
      } else if (rebootedAt && Date.now() - rebootedAt > 25000) {
        fail('设备回来了，但版本仍是 ' + (baseVersion || '旧版本') + '。',
             '镜像没有被采用：校验失败，或新固件启动后自检未通过、引导程序已回滚。设备现在是安全的，可以重新上传。');
      }
    }
    if (currentTab === 'wifi') { pollProv(); }
  }, 2000);
})();
