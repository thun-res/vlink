/* =========================================================================
   VLink Official Site — main.js
   - IntersectionObserver reveal
   - Animated number counter (0 → target, with optional "+" suffix via CSS)
   - Quickstart code tabs (C++ Publisher / Subscriber)
   - Quickstart live log (loops forever, starts on viewport entry)
   - CLI terminal gallery (4 representative panels: monitor / list / check / bag)
   - QQ group modal + clipboard copy
   - Smooth scroll for hash links with sticky-header offset
   ========================================================================= */
'use strict';

/* ---------- 1. Reveal on scroll ---------- */
(() => {
  const els = document.querySelectorAll('.reveal');
  if (!('IntersectionObserver' in window)) { els.forEach(e => e.classList.add('in')); return; }
  const io = new IntersectionObserver((entries) => {
    entries.forEach(en => {
      if (en.isIntersecting) { en.target.classList.add('in'); io.unobserve(en.target); }
    });
  }, { threshold: 0.12, rootMargin: '0px 0px -40px 0px' });
  els.forEach(e => io.observe(e));
})();

/* ---------- 2. Number counter ---------- */
(() => {
  const targets = document.querySelectorAll('[data-count-target]');
  if (!targets.length) return;
  const io = new IntersectionObserver((entries) => {
    entries.forEach(en => {
      if (!en.isIntersecting) return;
      const el = en.target;
      const end = +el.dataset.countTarget;
      const dur = 1200;
      const t0 = performance.now();
      const tick = (t) => {
        const p = Math.min(1, (t - t0) / dur);
        const v = Math.round(end * (1 - Math.pow(1 - p, 3)));
        el.textContent = v;
        if (p < 1) requestAnimationFrame(tick);
      };
      requestAnimationFrame(tick);
      io.unobserve(el);
    });
  }, { threshold: 0.35 });
  targets.forEach(t => io.observe(t));
})();

/* ---------- 3. Quickstart code (both snippets rendered at once) ---------- */
(() => {
  const pubStage = document.getElementById('qs-body-pub');
  const subStage = document.getElementById('qs-body-sub');
  if (!pubStage || !subStage) return;

  const esc = (s) => s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');

  // Token-based highlighter — avoids the classic "//  inside a URL string is
  // mistaken for a comment" bug by tokenizing left-to-right and skipping
  // already-classified characters.
  const hl = (src) => {
    // Operate on the escaped string; tokens emit HTML directly.
    const code = esc(src);
    const KW  = /^(?:#include|#define|auto|int|bool|char|float|double|long|short|size_t|unsigned|const|constexpr|void|while|for|return|if|else|do|switch|case|break|continue|class|struct|template|typename|using|namespace|true|false|static_assert|new|delete|this|nullptr|static|inline)\b/;
    const NS  = /^(?:vlink|std|pb)\b/;     // namespaces — coloured like keywords but distinct
    const TPL = /^(?:Publisher|Subscriber|Client|Server|Setter|Getter|SecurityPublisher|SecuritySubscriber|CameraFrame|PointCloud|Imu|Gear|Req|Resp|Point2D|MyMessage|SensorReading|MessageLoop|Timer|Bytes|Sensor)\b/;
    // Match VLink logging macros: VLOG_/MLOG_/CLOG_/SLOG_ + one level letter.
    const MACRO = /^(?:VLOG|MLOG|CLOG|SLOG)_[A-Z]\b/;
    let out = '';
    let i = 0;
    const N = code.length;
    while (i < N) {
      const rest = code.slice(i);
      // 0. `#include <header>` — color the header like a string literal so that
      //    the path (e.g. "vlink/vlink.h") isn't mis-tokenised as the vlink
      //    namespace + `.h` method call. `<` / `>` are already escaped to
      //    `&lt;` / `&gt;` by esc(), so match those forms here.
      const incM = rest.match(/^#include(\s+)&lt;([^\n]*?)&gt;/);
      if (incM) {
        out += '<span class="kw">#include</span>' + incM[1] +
               '<span class="str">&lt;' + incM[2] + '&gt;</span>';
        i += incM[0].length;
        continue;
      }
      // 1. line comment //
      if (rest.startsWith('//')) {
        const nl = rest.indexOf('\n');
        const seg = nl === -1 ? rest : rest.slice(0, nl);
        out += '<span class="comment">' + seg + '</span>';
        i += seg.length;
        continue;
      }
      // 2. string literal  "..."
      //    esc() never escapes the quote character, so the token remains a
      //    plain ". We must match against " (not &quot;), otherwise URLs
      //    like "shm://foo" get mis-tokenised — the // gets picked up as a
      //    line comment and the whole string turns grey.
      if (code[i] === '"') {
        let j = i + 1;
        while (j < N && code[j] !== '"' && code[j] !== '\n') {
          if (code[j] === '\\') j += 2; else j++;
        }
        const end = (j < N && code[j] === '"') ? j + 1 : j;
        out += '<span class="str">' + code.slice(i, end) + '</span>';
        i = end;
        continue;
      }
      // 3. char literal '...'
      if (code[i] === "'") {
        let j = i + 1;
        while (j < N && code[j] !== "'" && code[j] !== '\n') {
          if (code[j] === '\\') j += 2; else j++;
        }
        out += '<span class="str">' + code.slice(i, j + 1) + '</span>';
        i = j + 1;
        continue;
      }
      // 4. numeric literal
      const numM = rest.match(/^(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?[fFuUlL]?/);
      if (numM) {
        out += '<span class="num-lit">' + numM[0] + '</span>';
        i += numM[0].length;
        continue;
      }
      // 5. word: keyword / namespace / template / macro / method / identifier
      const wordM = rest.match(/^[A-Za-z_#][A-Za-z_0-9]*/);
      if (wordM) {
        const w = wordM[0];
        if (KW.test(w)) {
          out += '<span class="kw">' + w + '</span>';
        } else if (NS.test(w)) {
          out += '<span class="ns">' + w + '</span>';
        } else if (TPL.test(w)) {
          out += '<span class="tpl">' + w + '</span>';
        } else if (MACRO.test(w)) {
          out += '<span class="macro">' + w + '</span>';
        } else if (i > 0 && (code[i - 1] === '.' ||
                             (code[i - 1] === ':' && code[i - 2] === ':'))) {
          // method call after `.xxx` or `::xxx`
          out += '<span class="fn">' + w + '</span>';
        } else {
          out += w;
        }
        i += w.length;
        continue;
      }
      // 6. fallback: 1 char
      out += code[i];
      i++;
    }
    return out;
  };

  const snippets = {
    'cpp-pub': {
      title: 'publisher.cc — publishes to dds://sensor/imu',
      code:
`// Run alongside subscriber.cc — same URL, they pair automatically.
#include <vlink/vlink.h>
#include "proto/sensor.pb.h"        // protobuf-generated header

int main() {
  vlink::Publisher<pb::Sensor::Imu> pub("dds://sensor/imu");

  for (int seq = 0; ; ++seq) {
    pb::Sensor::Imu msg;
    msg.set_sequence(seq);
    msg.set_acc_x(0.01 * seq);
    pub.publish(msg);
    MLOG_I("[Publisher]  published seq=#{}", seq);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  ...
}`
    },
    'cpp-sub': {
      title: 'subscriber.cc — receives from dds://sensor/imu',
      code:
`// Run alongside publisher.cc — picks up every sample on the same URL.
#include <vlink/vlink.h>
#include "proto/sensor.pb.h"        // protobuf-generated header

int main() {
  vlink::Subscriber<pb::Sensor::Imu> sub("dds://sensor/imu");
  sub.listen([](const pb::Sensor::Imu& m) {
    MLOG_I("[Subscriber] seq=#{} ax={}", m.sequence(), m.acc_x());
  });

  ...
}`
    },
  };

  pubStage.innerHTML = hl(snippets['cpp-pub'].code);
  subStage.innerHTML = hl(snippets['cpp-sub'].code);
})();

/* ---------- 3b. Quickstart live log (loops forever) ---------- */
(() => {
  const host = document.getElementById('qs-log');
  if (!host) return;

  const SEQ = 10;
  const makeCycle = () => {
    const out = [];
    for (let i = 0; i < SEQ; i++) {
      const ax = (0.01 * i).toFixed(2);
      out.push(`[Publisher]  published seq=#${i}`);
      out.push(`[Subscriber] seq=#${i} ax=${ax}`);
    }
    return out;
  };

  let lines = makeCycle();
  let idx = 0;

  const pushLine = () => {
    if (idx >= lines.length) {
      // hold the finished log for a beat, then restart
      setTimeout(() => {
        host.innerHTML = '';
        idx = 0;
        lines = makeCycle();
        pushLine();
      }, 2000);
      return;
    }
    host.insertAdjacentHTML('beforeend', lines[idx] + '\n');
    host.scrollTop = host.scrollHeight;
    idx++;
    // Strict 500 ms publish cadence: Publisher fires, Subscriber receives
    // ~40 ms later, then we wait ~460 ms for the next Publisher tick.
    // (idx is now odd right after a Publisher line, even after a Subscriber.)
    const wait = idx % 2 === 1 ? 460 : 40;
    setTimeout(pushLine, wait);
  };

  // Start only when the element scrolls into view (save CPU + cache warm)
  const io = new IntersectionObserver((ens) => {
    ens.forEach(en => {
      if (en.isIntersecting) { pushLine(); io.disconnect(); }
    });
  }, { threshold: 0.25 });
  io.observe(host);
})();

/* ---------- 4. CLI terminal gallery ---------- */
(() => {
  const panelBodies = Array.from(document.querySelectorAll('[data-cli-panel]'));
  if (!panelBodies.length) return;

  const KB = 1024;
  const MB = 1024 * 1024;
  const clamp = (value, min, max) => Math.max(min, Math.min(max, value));
  const escapeTerminalText = (text) => String(text)
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;');
  const prompt = (cmd) => `<span class="head">$ ${escapeTerminalText(cmd)}</span>`;
  const shellReadyLine = () => `<span class="head">$ </span><span class="term-cursor">█</span>`;
  const renderCursor = (elapsedMs) =>
    `<span class="term-cursor">${Math.floor(elapsedMs / 530) % 2 === 0 ? '█' : '&nbsp;'}</span>`;
  const typedPromptLine = (cmd, elapsedMs, msPerChar = 42) => {
    const visibleCount = clamp(Math.floor(elapsedMs / msPerChar), 0, cmd.length);
    return `<span class="head">$ ${escapeTerminalText(cmd.slice(0, visibleCount))}</span>${renderCursor(elapsedMs)}`;
  };
  const commandTypingDuration = (cmd, msPerChar = 42) => (cmd.length * msPerChar) + 180;
  const fit = (value, width) => {
    const text = String(value);
    if (width <= 0) return '';
    return text.length >= width ? text.slice(0, width) : text + ' '.repeat(width - text.length);
  };
  const clip = (value, width) => {
    const text = String(value);
    if (width <= 0) return '';
    return text.length > width ? text.slice(0, width) : text;
  };
  const padScreen = (lines, rows) => {
    const output = lines.slice(0, rows);
    while (output.length < rows) output.push('');
    return output;
  };
  const trimScreenPadding = (lines) => {
    let end = lines.length;
    while (end > 0 && lines[end - 1] === '') end -= 1;
    return lines.slice(0, end);
  };
  const withExitPrompt = (lines, rows) => padScreen([...lines, '', shellReadyLine()], rows);
  const composeScreen = (headLines, bodyLines, footerLine, rows) => {
    const bodyRows = Math.max(rows - headLines.length - (footerLine ? 1 : 0), 0);
    const visible = bodyLines.slice(0, bodyRows);
    while (visible.length < bodyRows) visible.push('');
    return [...headLines, ...visible, ...(footerLine ? [footerLine] : [])];
  };
  const footerToggle = (char, enabled) => enabled ? `<span class="on">${char}</span>` : char;
  const formatFixed = (value, digits = 2) => Number(value).toFixed(digits);
  const formatRateSize = (bytesPerSec) => {
    const value = Math.max(bytesPerSec, 0);
    if (value >= 1024 * 1024 * 1024) return `${formatFixed(value / 1024 / 1024 / 1024)}GB/s`;
    if (value >= 1024 * 1024) return `${formatFixed(value / 1024 / 1024)}MB/s`;
    if (value >= 1024) return `${formatFixed(value / 1024)}KB/s`;
    return `${formatFixed(value)}B/s`;
  };
  const formatFreq = (value) => `${formatFixed(value)}Hz`;
  const formatLoss = (value) => `${formatFixed(value, value > 0 && value < 0.0001 ? 4 : 2)}%`;
  const formatLatency = (value) => value == null ? '---' : `${formatFixed(value)}ms`;
  const formatClock = (seconds) => {
    const safe = Math.max(seconds, 0);
    const h = Math.floor(safe / 3600);
    const m = Math.floor((safe % 3600) / 60);
    const s = Math.floor(safe % 60);
    return [h, m, s].map((item) => String(item).padStart(2, '0')).join(':');
  };
  const formatElapsed = (seconds) => `${formatFixed(seconds)}s`;
  const formatUtcDate = (millisecondsSinceEpoch) => {
    const date = new Date(millisecondsSinceEpoch);
    const Y = date.getUTCFullYear();
    const M = String(date.getUTCMonth() + 1).padStart(2, '0');
    const D = String(date.getUTCDate()).padStart(2, '0');
    const h = String(date.getUTCHours()).padStart(2, '0');
    const m = String(date.getUTCMinutes()).padStart(2, '0');
    const s = String(date.getUTCSeconds()).padStart(2, '0');
    const ms = String(date.getUTCMilliseconds()).padStart(3, '0');
    return `${Y}-${M}-${D} ${h}:${m}:${s}.${ms}`;
  };
  const formatChartValue = (value, unitValue = 1000) => {
    const abs = Math.max(value, 0);
    if (abs >= unitValue * unitValue * unitValue) return `${formatFixed(abs / unitValue / unitValue / unitValue, 1)}G`;
    if (abs >= unitValue * unitValue) return `${formatFixed(abs / unitValue / unitValue, 1)}M`;
    if (abs >= unitValue) return `${formatFixed(abs / unitValue, 1)}K`;
    if (abs >= 100) return `${formatFixed(abs, 0)}`;
    if (abs >= 10) return `${formatFixed(abs, 1)}`;
    return `${formatFixed(abs, 2)}`;
  };
  const chunkPages = (lines, pageSize) => {
    const pages = [];
    for (let i = 0; i < lines.length; i += pageSize) {
      pages.push(lines.slice(i, i + pageSize));
    }
    return pages.length ? pages : [[]];
  };
  const formatProgressBar = (progress) => {
    const total = 50;
    const filled = clamp(Math.round(progress * total), 0, total);
    const empty = total - filled;
    return `Progress: [<span class="ok">${'#'.repeat(filled)}</span><span class="dim">${'-'.repeat(empty)}</span>] ${formatFixed(progress * 100)} % `;
  };
  const noise = (seed, step, scale = 1, period = 10, phase = 0) =>
    Math.sin((step + seed) / period + phase) * scale;
  const measureProbeMap = new WeakMap();
  const measureTerminal = (bodyEl) => {
    const style = getComputedStyle(bodyEl);
    let probe = measureProbeMap.get(bodyEl);
    if (!probe || !probe.isConnected) {
      probe = document.createElement('span');
      probe.textContent = 'MMMMMMMMMM';
      probe.style.position = 'absolute';
      probe.style.visibility = 'hidden';
      probe.style.pointerEvents = 'none';
      probe.style.whiteSpace = 'pre';
      document.body.appendChild(probe);
      measureProbeMap.set(bodyEl, probe);
    }
    probe.style.font = style.font;
    const probeWidth = Math.max(probe.getBoundingClientRect().width / 10, 1);
    const lineHeight = parseFloat(style.lineHeight) || (parseFloat(style.fontSize) * 1.65);
    const padX = parseFloat(style.paddingLeft) + parseFloat(style.paddingRight);
    const padY = parseFloat(style.paddingTop) + parseFloat(style.paddingBottom);
    const cols = Math.max(48, Math.floor((bodyEl.clientWidth - padX) / probeWidth));
    const rows = Math.max(12, Math.floor((bodyEl.clientHeight - padY) / lineHeight));
    return { cols, rows, innerWidth: bodyEl.clientWidth - padX };
  };

  const TypeBits = { pub: 1, sub: 2, set: 4, get: 8, ser: 16, cli: 32 };
  const monitorTypeView = (bits) => {
    switch (bits) {
      case TypeBits.pub | TypeBits.sub: return 'Pub|Sub';
      case TypeBits.set | TypeBits.get: return 'Set|Get';
      case TypeBits.ser | TypeBits.cli: return 'Ser|Cli';
      case TypeBits.pub: return 'Pub|---';
      case TypeBits.sub: return '---|Sub';
      case TypeBits.set: return 'Set|---';
      case TypeBits.get: return '---|Get';
      case TypeBits.ser: return 'Ser|---';
      case TypeBits.cli: return '---|Cli';
      case TypeBits.pub | TypeBits.get: return 'Pub|Get';
      case TypeBits.set | TypeBits.sub: return 'Set|Sub';
      default:
        if (bits === (TypeBits.pub | TypeBits.set)) return 'Pub|---';
        if (bits === (TypeBits.pub | TypeBits.set | TypeBits.sub)) return 'Pub|Sub';
        if (bits === (TypeBits.pub | TypeBits.set | TypeBits.get)) return 'Pub|Get';
        if (bits === (TypeBits.pub | TypeBits.set | TypeBits.sub | TypeBits.get)) return 'Pub|Sub';
        if (bits === (TypeBits.sub | TypeBits.get)) return '---|Sub';
        if (bits === (TypeBits.sub | TypeBits.get | TypeBits.pub)) return 'Pub|Sub';
        if (bits === (TypeBits.sub | TypeBits.get | TypeBits.set)) return 'Set|Sub';
        return '---|---';
    }
  };
  const monitorTransportSortIndex = (url) => {
    if (url.startsWith('intra://')) return 1;
    if (url.startsWith('shm://')) return 2;
    if (url.startsWith('shm2://')) return 3;
    if (url.startsWith('zenoh://')) return 4;
    if (url.startsWith('dds://')) return 5;
    if (url.startsWith('ddsc://')) return 6;
    if (url.startsWith('ddsr://')) return 7;
    if (url.startsWith('ddst://')) return 8;
    if (url.startsWith('someip://')) return 9;
    if (url.startsWith('mqtt://')) return 10;
    if (url.startsWith('fdbus://')) return 11;
    if (url.startsWith('qnx://')) return 12;
    return 99;
  };
  const makeMonitorRow = (typeBits, url, ser, baseFreq, baseRate, baseLatency, activity = 'active', baseLoss = 0) => ({
    typeBits,
    type: monitorTypeView(typeBits),
    sortIndex: monitorTransportSortIndex(url),
    url,
    ser,
    baseFreq,
    baseRate,
    baseLatency,
    baseLoss,
    activity,
    passive: (typeBits & (TypeBits.pub | TypeBits.set)) === 0,
  });
  const monitorRows = (() => {
    const rows = [];
    const add = (typeBits, url, ser, freq, rate, latency, activity = 'active', loss = 0) => {
      rows.push(makeMonitorRow(typeBits, url, ser, freq, rate, latency, activity, loss));
    };
    const addPub = (url, ser, freq, rate, latency, activity = 'active', loss = 0) =>
      add(TypeBits.pub, url, ser, freq, rate, latency, activity, loss);
    const addSub = (url, ser) => add(TypeBits.sub, url, ser, 0, 0, null, 'white');
    const addPubSub = (url, ser, freq, rate, latency, activity = 'active', loss = 0) =>
      add(TypeBits.pub | TypeBits.sub, url, ser, freq, rate, latency, activity, loss);
    const addSet = (url, ser, freq, rate, latency, activity = 'warm', loss = 0) =>
      add(TypeBits.set, url, ser, freq, rate, latency, activity, loss);
    const addGet = (url, ser) => add(TypeBits.get, url, ser, 0, 0, null, 'white');
    const addSetGet = (url, ser, freq, rate, latency, activity = 'warm', loss = 0) =>
      add(TypeBits.set | TypeBits.get, url, ser, freq, rate, latency, activity, loss);
    const addServer = (url, ser) => add(TypeBits.ser, url, ser, 0, 0, null, 'white');
    const addClient = (url, ser) => add(TypeBits.cli, url, ser, 0, 0, null, 'white');

    [
      ['shm://camera/front', 'raw.CameraFrame', 30, 89.50 * MB, 2.40, 'active'],
      ['shm://camera/left', 'raw.CameraFrame', 30, 88.95 * MB, 2.31, 'active'],
      ['shm://camera/rear', 'raw.CameraFrame', 30, 88.70 * MB, 2.36, 'warm'],
      ['shm://camera/right', 'raw.CameraFrame', 30, 88.88 * MB, 2.33, 'active'],
      ['shm://camera/fisheye/front_left', 'raw.CameraFrame', 30, 41.28 * MB, 2.58, 'active'],
      ['shm://camera/fisheye/front_right', 'raw.CameraFrame', 30, 41.11 * MB, 2.61, 'active'],
      ['shm://camera/fisheye/rear_left', 'raw.CameraFrame', 30, 41.04 * MB, 2.57, 'active'],
      ['shm://camera/fisheye/rear_right', 'raw.CameraFrame', 30, 40.97 * MB, 2.60, 'warm'],
      ['shm://camera/narrow/front', 'raw.CameraFrame', 20, 24.82 * MB, 2.72, 'active'],
      ['shm://camera/narrow/rear', 'raw.CameraFrame', 20, 24.67 * MB, 2.75, 'warm'],
      ['shm://lidar/diagnostic/front', 'pb.LidarDiagnostic', 50, 60 * KB, 0.25, 'active'],
      ['shm://lidar/diagnostic/rear', 'pb.LidarDiagnostic', 50, 58 * KB, 0.27, 'warm'],
      ['shm://lidar/imu/front', 'pb.Imu', 200, 180 * KB, 0.19, 'active'],
      ['shm://lidar/imu/rear', 'pb.Imu', 200, 176 * KB, 0.21, 'active'],
      ['shm://lidar/points/front', 'zerocopy.PointCloud', 10, 15.20 * MB, 0.90, 'active'],
      ['shm://lidar/points/left', 'zerocopy.PointCloud', 10, 15.06 * MB, 0.93, 'active'],
      ['shm://lidar/points/rear', 'zerocopy.PointCloud', 10, 15.08 * MB, 0.92, 'active'],
      ['shm://lidar/points/right', 'zerocopy.PointCloud', 10, 15.05 * MB, 0.94, 'warm'],
      ['shm://lidar/points/roof', 'zerocopy.PointCloud', 10, 15.11 * MB, 0.88, 'active'],
      ['shm://lidar/state/front', 'pb.LidarState', 10, 36 * KB, 0.55, 'warm'],
      ['shm://radar/corner/front_left', 'pb.RadarTrackArray', 20, 1.95 * MB, 1.54, 'active'],
      ['shm://radar/corner/front_right', 'pb.RadarTrackArray', 20, 1.94 * MB, 1.56, 'active'],
      ['shm://radar/corner/rear_left', 'pb.RadarTrackArray', 20, 1.90 * MB, 1.58, 'active'],
      ['shm://radar/corner/rear_right', 'pb.RadarTrackArray', 20, 1.91 * MB, 1.57, 'active'],
      ['shm://radar/front', 'pb.RadarTrackArray', 20, 2.25 * MB, 1.48, 'active'],
      ['shm://radar/rear', 'pb.RadarTrackArray', 20, 2.11 * MB, 1.55, 'stale'],
      ['shm://radar/side_left', 'pb.RadarTrackArray', 20, 2.03 * MB, 1.49, 'active'],
      ['shm://radar/side_right', 'pb.RadarTrackArray', 20, 2.02 * MB, 1.51, 'active'],
      ['shm://ultrasonic/array', 'pb.UltrasonicArray', 15, 180 * KB, 0.66, 'active'],
      ['shm://ultrasonic/front_array', 'pb.UltrasonicArray', 15, 140 * KB, 0.61, 'active'],
      ['shm://ultrasonic/rear_array', 'pb.UltrasonicArray', 15, 140 * KB, 0.63, 'warm'],
      ['shm://ultrasonic/side_array', 'pb.UltrasonicArray', 15, 110 * KB, 0.60, 'active'],
      ['shm://vehicle/battery', 'pb.VehicleBattery', 2, 240, 0.31, 'warm'],
      ['shm://vehicle/brake_state', 'pb.BrakeState', 50, 18 * KB, 0.28, 'stale'],
      ['shm://vehicle/chassis', 'pb.VehicleChassis', 100, 120 * KB, 0.22, 'active'],
      ['shm://vehicle/gnss/raw_fix', 'pb.GnssRawFix', 10, 32 * KB, 0.36, 'active'],
      ['shm://vehicle/imu', 'pb.VehicleImu', 200, 1.24 * MB, 0.18, 'active'],
      ['shm://vehicle/odometer', 'pb.VehicleOdometer', 100, 31.20 * KB, 0.23, 'active'],
      ['shm://vehicle/steer_state', 'pb.SteerState', 50, 16 * KB, 0.29, 'warm'],
      ['shm://vehicle/suspension', 'pb.SuspensionState', 50, 20 * KB, 0.27, 'active'],
      ['shm://vehicle/tire_pressure', 'pb.TirePressure', 1, 180, 0.34, 'active'],
      ['shm://vehicle/wheel_speed', 'pb.WheelSpeed', 100, 100 * KB, 0.24, 'active'],
      ['shm://vehicle/wiper', 'pb.WiperState', 1, 160, 0.20, 'active'],
      ['dds://decision/context', 'pb.DecisionContext', 10, 70 * KB, 1.52, 'active'],
      ['dds://decision/path', 'pb.PlanningPath', 10, 220 * KB, 2.10, 'active'],
      ['dds://diagnostics/fault', 'pb.FaultReport', 2, 30 * KB, 0.64, 'active'],
      ['dds://diagnostics/log', 'pb.LogEvent', 2, 50 * KB, 0.40, 'warm'],
      ['dds://fusion/tracks', 'pb.FusionTracks', 20, 1.95 * MB, 4.02, 'active', 0.01],
      ['dds://hmi/status', 'pb.HmiStatus', 1, 10 * KB, 0.88, 'active'],
      ['dds://localization/anchor', 'pb.LocalizationAnchor', 1, 10 * KB, 0.92, 'active'],
      ['dds://localization/pose', 'pb.LocalizationPose', 50, 0.80 * MB, 0.82, 'active'],
      ['dds://mapping/anchor_grid', 'pb.AnchorGrid', 2, 50 * KB, 1.84, 'active'],
      ['dds://mapping/local_map', 'pb.LocalMap', 5, 0.42 * MB, 2.85, 'stale'],
      ['dds://nav/gnss/fix', 'pb.GnssFix', 10, 24 * KB, 0.74, 'active'],
      ['dds://nav/imu', 'pb.Imu', 200, 210 * KB, 0.42, 'active'],
      ['dds://nav/ins', 'pb.InsState', 100, 80 * KB, 0.48, 'warm'],
      ['dds://nav/odom', 'pb.Odometry', 50, 60 * KB, 0.62, 'active'],
      ['dds://nav/time_sync', 'pb.TimeSync', 1, 6 * KB, 0.12, 'active'],
      ['dds://routing/reference', 'pb.RouteReference', 1, 20 * KB, 0.95, 'active'],
    ].forEach((item) => addPub(...item));

    [
      ['dds://control/command', 'pb.ControlCommand'],
      ['dds://control/emergency_stop', 'pb.EmergencyStop'],
      ['dds://map/query', 'pb.MapQueryReq'],
      ['dds://planner/request', 'pb.PlanningRequest'],
      ['dds://planner/reset', 'pb.PlannerReset'],
      ['dds://safety/override', 'pb.SafetyOverride'],
      ['dds://simulation/inject', 'pb.SimInject'],
      ['shm://param/route_id', 'pb.RouteId'],
      ['shm://param/vehicle_mode', 'pb.VehicleMode'],
      ['shm://service/upgrade/request', 'pb.UpgradeRequest'],
      ['shm://tools/capture', 'pb.CaptureCommand'],
      ['shm://vehicle/teleop_request', 'pb.TeleopRequest'],
    ].forEach((item) => addSub(...item));

    [
      ['dds://control/chassis_feedback', 'pb.ChassisFeedback', 100, 80 * KB, 0.32, 'active'],
      ['dds://control/feedback', 'pb.ControlFeedback', 100, 80 * KB, 0.34, 'active'],
      ['dds://decision/command', 'pb.DecisionCommand', 20, 50 * KB, 1.19, 'active'],
      ['dds://diagnostics/channel', 'pb.DiagnosticChannel', 5, 12 * KB, 0.45, 'warm'],
      ['dds://planning/state', 'pb.PlannerState', 5, 30 * KB, 1.26, 'active'],
      ['dds://visualization/fusion_debug', 'pb.FusionDebug', 10, 120 * KB, 1.40, 'active'],
      ['shm://vehicle/brake_state_sync', 'pb.BrakeState', 50, 18 * KB, 0.27, 'active'],
      ['shm://vehicle/gear_state_sync', 'pb.GearState', 10, 2 * KB, 0.21, 'warm'],
      ['shm://vehicle/light_state_sync', 'pb.LightState', 5, 2 * KB, 0.19, 'active'],
      ['shm://vehicle/power_state_sync', 'pb.PowerMode', 1, 180, 0.35, 'warm'],
    ].forEach((item) => addPubSub(...item));

    [
      ['shm://vehicle/brake', 'pb.BrakeCmd', 0.50, 160, 0.56, 'warm'],
      ['shm://vehicle/gear', 'pb.GearCmd', 0.67, 120, 0.56, 'warm'],
      ['shm://vehicle/horn', 'pb.HornCmd', 0.0, 0, null, 'stale'],
      ['shm://vehicle/light', 'pb.LightCmd', 0.33, 140, 0.58, 'warm'],
      ['shm://vehicle/power_mode', 'pb.PowerMode', 0.20, 64, 0.41, 'active'],
      ['shm://vehicle/state', 'pb.VehicleStateCmd', 1.0, 180, 0.44, 'active'],
      ['shm://vehicle/steering', 'pb.SteeringCmd', 0.67, 220, 0.63, 'warm'],
      ['shm://vehicle/throttle', 'pb.ThrottleCmd', 0.50, 180, 0.57, 'active'],
      ['dds://config/active_route', 'pb.RouteConfig', 0.10, 64, 0.40, 'warm'],
      ['dds://config/profile', 'pb.RuntimeProfile', 0.05, 48, 0.42, 'active'],
    ].forEach((item) => addSet(...item));

    [
      ['dds://config/active_route', 'pb.RouteConfig'],
      ['dds://map/tile_cache', 'pb.MapTileCache'],
      ['dds://planner/profile', 'pb.PlannerProfile'],
      ['shm://param/vehicle_mode', 'pb.VehicleMode'],
      ['shm://service/upgrade/state', 'pb.UpgradeState'],
      ['shm://tools/capture/state', 'pb.CaptureState'],
    ].forEach((item) => addGet(...item));

    [
      ['shm://vehicle/assist_level', 'pb.AssistLevel', 0.20, 48, 0.45, 'warm'],
      ['shm://vehicle/parking_mode', 'pb.ParkingMode', 0.10, 48, 0.46, 'active'],
      ['dds://config/calibration', 'pb.CalibrationConfig', 0.05, 44, 0.50, 'warm'],
      ['dds://config/mission_mode', 'pb.MissionMode', 0.05, 44, 0.49, 'active'],
    ].forEach((item) => addSetGet(...item));

    [
      ['dds://map/service', 'pb.MapQueryResp'],
      ['dds://planner/service', 'pb.PlanningServiceResp'],
      ['dds://routing/service', 'pb.RouteServiceResp'],
      ['shm://maintenance/service', 'pb.MaintenanceResp'],
      ['shm://tools/service', 'pb.ToolsServiceResp'],
    ].forEach((item) => addServer(...item));

    [
      ['dds://map/client', 'pb.MapQueryReq'],
      ['dds://planner/client', 'pb.PlanningServiceReq'],
      ['dds://routing/client', 'pb.RouteServiceReq'],
      ['shm://maintenance/client', 'pb.MaintenanceReq'],
      ['shm://tools/client', 'pb.ToolsServiceReq'],
    ].forEach((item) => addClient(...item));

    const sorted = rows.sort((lhs, rhs) =>
      lhs.sortIndex - rhs.sortIndex ||
      lhs.url.localeCompare(rhs.url) ||
      lhs.typeBits - rhs.typeBits ||
      lhs.ser.localeCompare(rhs.ser));
    const merged = [];
    sorted.forEach((row) => {
      const last = merged[merged.length - 1];
      if (last && last.url === row.url) {
        last.typeBits |= row.typeBits;
        last.type = monitorTypeView(last.typeBits);
        if (last.passive && !row.passive) {
          last.baseFreq = row.baseFreq;
          last.baseRate = row.baseRate;
          last.baseLatency = row.baseLatency;
          last.baseLoss = row.baseLoss;
          last.activity = row.activity;
        }
        last.passive = (last.typeBits & (TypeBits.pub | TypeBits.set)) === 0;
        if ((!last.ser || last.ser === 'Bytes') && row.ser && row.ser !== 'Bytes') {
          last.ser = row.ser;
        }
        return;
      }
      merged.push({ ...row });
    });
    return merged;
  })();
  const firstEprotoTargetUrl = 'shm://lidar/points/front';
  const secondEprotoTargetUrl = 'dds://nav/gnss/fix';
  const firstEprotoIndex = monitorRows.findIndex((row) => row.url === firstEprotoTargetUrl);
  const secondEprotoIndex = monitorRows.findIndex((row) => row.url === secondEprotoTargetUrl);

  const listProcesses = [
    {
      name: 'chassis_gateway',
      pid: 4302,
      host: 'xavier',
      ip: '192.168.1.10',
      publisher: [['shm://vehicle/chassis', 'pb.VehicleChassis'], ['shm://vehicle/imu', 'pb.VehicleImu']],
      setter: [['shm://vehicle/brake', 'pb.BrakeCmd'], ['shm://vehicle/steering', 'pb.SteeringCmd']],
    },
    {
      name: 'control_node',
      pid: 4380,
      host: 'xavier',
      ip: '192.168.1.10',
      publisher: [['dds://control/feedback', 'pb.ControlFeedback']],
      subscriber: [['dds://decision/command', 'pb.DecisionCommand']],
      setter: [['shm://vehicle/brake', 'pb.BrakeCmd'], ['shm://vehicle/throttle', 'pb.ThrottleCmd']],
    },
    {
      name: 'localization_node',
      pid: 4318,
      host: 'xavier',
      ip: '192.168.1.10',
      publisher: [['dds://localization/pose', 'pb.LocalizationPose']],
      subscriber: [['shm://vehicle/imu', 'pb.VehicleImu'], ['shm://vehicle/odometer', 'pb.VehicleOdometer']],
    },
    {
      name: 'map_service',
      pid: 4326,
      host: 'xavier',
      ip: '192.168.1.10',
      server: [['dds://map/service', 'pb.MapQueryResp']],
      getter: [['dds://config/active_route', 'pb.RouteConfig']],
    },
    {
      name: 'perception_fusion',
      pid: 4361,
      host: 'xavier',
      ip: '192.168.1.10',
      publisher: [['dds://fusion/tracks', 'pb.FusionTracks']],
      subscriber: [['shm://camera/front', 'raw.CameraFrame'], ['shm://lidar/points/front', 'zerocopy.PointCloud']],
    },
    {
      name: 'planner_node',
      pid: 4348,
      host: 'xavier',
      ip: '192.168.1.10',
      client: [['dds://map/client', 'pb.MapQueryReq']],
      publisher: [['dds://decision/path', 'pb.PlanningPath']],
      subscriber: [['dds://fusion/tracks', 'pb.FusionTracks'], ['dds://localization/pose', 'pb.LocalizationPose']],
    },
  ].sort((lhs, rhs) => lhs.name.localeCompare(rhs.name));

  const checkDiagItems = [
    ['* Check available IP addresses...', 'ok', 'Found 3 IP Address', 100],
    ['* Check VLink DDS IP available...', 'ok', '192.168.1.10 is valid', 100],
    ['* Check VLink multicast address...', 'ok', 'Found 239.255.0.100', 100],
    ['* Check DDS multicast address...', 'warn', 'Cannot find 239.255.0.1', 100],
    ['* Check available space for log dir...', 'ok', 'Available: 48.25GB', 500],
    ['* Check cpu usage...', 'ok', 'Usage 18.42%', 100],
    ['* Check memory usage...', 'warn', 'Usage 61.37%', 100],
    ['* Check proxy running...', 'err', 'Proxy is not running', 100],
    ['* Check bag running...', 'ok', 'Bag is not running', 100],
    ['* Check dump running...', 'ok', 'Dump is not running', 100],
    ['* Check eproto running...', 'ok', 'Eproto is not running', 100],
    ['* Check monitor running...', 'warn', 'Monitor is running', 100],
    ['* Check viewer running...', 'ok', 'Viewer is not running', 100],
    ['* Check player running...', 'ok', 'Player is not running', 100],
    ['* Check analyzer running...', 'ok', 'Analyzer is not running', 100],
    ['* Check others running...', 'ok', 'No vlink user process running', 100],
  ];
  const checkEnvItems = [
    ['VLINK_PROTO_DIR', '/work/vlink/proto', 'Specifies the directory path where protocol buffer (.proto) files are stored.', true],
    ['VLINK_FBS_DIR', '/work/vlink/schema/fbs', 'Specifies the directory path where flatbuffers (.fbs) files are stored.', true],
    ['VLINK_SCHEMA_PLUGIN', '', 'Specifies the schema plugin used for protobuf/flatbuffers schema loading.', false],
    ['VLINK_TMP_DIR', '', 'Specifies the temporary directory folder.', false],
    ['VLINK_LOCK_DIR', '', 'Specifies the lock directory folder.', false],
    ['VLINK_LOG_LEVEL', '2', 'Sets log level (TRACE(0), DEBUG(1), INFO(2), WARN(3), ERROR(4), FATAL(5)).', true],
    ['VLINK_LOG_CONSOLE_LEVEL', '', 'Defines the log level for console output.', false],
    ['VLINK_LOG_FILE_LEVEL', '', 'Specifies the log level for file output.', false],
    ['VLINK_LOG_CONSOLE_UNORDER', '', 'Enable non-synchronized console output for better performance.', false],
    ['VLINK_LOG_CONSOLE_FMT', '', 'Set the console to output in a specific format.', false],
    ['VLINK_LOG_DIR', '', 'Directory path where log files will be stored.', false],
    ['VLINK_LOG_ENABLE_UTC', '', 'Set whether to use UTC time as the printed timestamp', false],
    ['VLINK_LOG_MAX_SIZE', '', 'Maximum size (in MB) of a single log file before rotation.', false],
    ['VLINK_LOG_MAX_COUNT', '', 'Maximum number of log files to retain after rotation.', false],
    ['VLINK_LOG_FLUSH_DELAY', '', 'Time delay (in milliseconds) to flush log buffers to storage.', false],
    ['VLINK_LOG_PLUGIN', '', 'Specifies a custom plugin for handling log output.', false],
    ['VLINK_LOG_STORE_STRATEGY', '', 'Specify the log storage policy.', false],
    ['VLINK_LOG_OPEN_APPEND', '', 'When the program starts, it appends the previous log entry.', false],
    ['VLINK_LOG_BLOCK_SYNC', '', 'User thread is blocked when queue is full until queue is consumed.', false],
    ['VLINK_LOG_WRITE_DEPTH', '', 'Set the depth of the log backend write queue.', false],
    ['VLINK_PLUGIN_DIR', '', 'Directory path where plugins are stored and loaded from.', false],
    ['VLINK_BAG_PATH', '', 'Path to store or load bag files for data recording or replay.', false],
    ['VLINK_BAG_TAG', '', 'A tag or identifier for the current data recording session.', false],
    ['VLINK_DISCOVER_DISABLE', '', 'Disables the system discovery feature when set to true.', false],
    ['VLINK_DISCOVER_NATIVE', '', 'Restricts discovery to localhost only.', false],
    ['VLINK_PROFILER_ENABLE', '', 'Enables the system profiler feature when set to true.', false],
    ['VLINK_QOS_CONFIG', '', 'Path to the configuration file for Quality of Service (QoS) settings.', false],
    ['VLINK_URL_PLUGINS', '', 'URL or path to load additional plugins from network or local resources.', false],
    ['VLINK_URL_REMAP', '', 'Configuration for remapping URLs or endpoints.', false],
    ['VLINK_INTRA_BIND', '', 'Specifies the binding address for Intra communication.', false],
    ['VLINK_DDS_BIND', '', 'Specifies the binding address for DDS communication.', false],
    ['VLINK_DDS_DEBUG', '', 'Enables or disables debug logs for DDS communication.', false],
    ['VLINK_DDS_EVENT_QOS', '', 'Quality of Service (QoS) settings for DDS events.', false],
    ['VLINK_DDS_METHOD_QOS', '', 'Quality of Service (QoS) settings for DDS methods.', false],
    ['VLINK_DDS_FIELD_QOS', '', 'Quality of Service (QoS) settings for DDS fields.', false],
    ['VLINK_DDS_DOMAIN', '', 'Specifies the DDS domain ID for communication.', false],
    ['VLINK_DDS_IP', '192.168.1.10', 'Sets the unicast IP address for DDS communication.', true],
    ['VLINK_DDS_IP_FILTER', '', 'Enables filtering to include only currently available addresses for DDS.', false],
    ['VLINK_DDS_MULTICAST_IP', '', 'Sets the multicast IP address for DDS communication.', false],
    ['VLINK_DDS_PEER', '', 'Configures the DDS peer-to-peer communication settings.', false],
    ['VLINK_DDS_BUF', '', 'Buffer size settings for DDS communication.', false],
    ['VLINK_DDS_MTU', '', 'Maximum message size (MTU) for DDS transport layer.', false],
    ['VLINK_DDS_UDP', '', 'Enables or configures UDP transport for DDS.', false],
    ['VLINK_DDS_TCP', '', 'Enables or configures TCP transport for DDS.', false],
    ['VLINK_DDS_SHM', '', 'Enables or configures shared memory transport for DDS.', false],
    ['VLINK_DDS_LESS_MEMORY', '', 'Enable DDS low memory usage mode.', false],
    ['VLINK_SHM_DEBUG', '', 'Enables debug information for shared memory transport.', false],
    ['VLINK_SHM_DEPTH', '', 'Configures the depth (queue size) of the shared memory transport buffer.', false],
    ['VLINK_SOMEIP_CFG', '', 'Path to the configuration file for SOME/IP communication.', false],
    ['VLINK_FASTDDS_QOS_FILE', '', 'Path to the QoS configuration file for Fast DDS.', false],
    ['VLINK_CYCLONEDDS_URI', '', 'URI or path to the configuration file for Cyclone DDS.', false],
  ];
  const bagInfoMetas = [
    ['Event', '9387', '2.45GB', '30.00Hz', '0.00%', 'shm://camera/front', 'raw.CameraFrame'],
    ['Event', '9379', '2.43GB', '30.00Hz', '0.00%', 'shm://camera/left', 'raw.CameraFrame'],
    ['Event', '9381', '2.43GB', '30.00Hz', '0.00%', 'shm://camera/right', 'raw.CameraFrame'],
    ['Event', '9380', '2.42GB', '30.00Hz', '0.00%', 'shm://camera/rear', 'raw.CameraFrame'],
    ['Event', '3127', '318MB', '10.00Hz', '0.00%', 'shm://lidar/points/front', 'zerocopy.PointCloud'],
    ['Event', '3128', '317MB', '10.00Hz', '0.00%', 'shm://lidar/points/rear', 'zerocopy.PointCloud'],
    ['Event', '3128', '316MB', '10.00Hz', '0.00%', 'shm://lidar/points/roof', 'zerocopy.PointCloud'],
    ['Event', '6240', '15.7MB', '20.00Hz', '0.00%', 'shm://radar/front', 'pb.RadarTrackArray'],
    ['Event', '6240', '15.5MB', '20.00Hz', '0.00%', 'shm://radar/rear', 'pb.RadarTrackArray'],
    ['Event', '31281', '9.5MB', '100.00Hz', '0.00%', 'shm://vehicle/odometer', 'pb.VehicleOdometer'],
    ['Event', '31281', '38.2MB', '100.00Hz', '0.00%', 'shm://vehicle/chassis', 'pb.VehicleChassis'],
    ['Field', '184', '14.1KB', '0.59Hz', '0.00%', 'shm://vehicle/gear', 'pb.GearCmd'],
    ['Field', '93', '7.3KB', '0.30Hz', '0.00%', 'shm://vehicle/light', 'pb.LightCmd'],
    ['Event', '1564', '2.2MB', '5.00Hz', '0.00%', 'dds://planning/state', 'pb.PlannerState'],
    ['Event', '624', '4.7MB', '2.00Hz', '0.01%', 'dds://diagnostics/log', 'pb.LogEvent'],
    ['Event', '3128', '46.4MB', '10.00Hz', '0.00%', 'dds://fusion/tracks', 'pb.FusionTracks'],
    ['Event', '15640', '12.1MB', '50.00Hz', '0.00%', 'dds://localization/pose', 'pb.LocalizationPose'],
  ];
  const MONITOR_COUNTER_CACHE = 2;
  const MONITOR_STALE_AFTER_SAMPLES = 2;
  const MONITOR_WARM_SAMPLES = 3;
  const monitorTransientProfiles = new Map([
    ['shm://radar/front', { dropAt: 2, recoverAt: 12 }],
    ['dds://nav/imu', { dropAt: 15, recoverAt: 25 }],
  ]);
  const getLowFreqPeriod = (freq) => clamp(Math.round(1 / Math.max(freq, 0.01)), 2, 8);
  const getLowFreqState = (row, sampleStep) => {
    const period = getLowFreqPeriod(row.baseFreq);
    const firstMessageStep = (period - ((row.url.length + 1) % period)) % period;
    if (sampleStep < firstMessageStep) {
      return { hasMessage: false, lastMessageAge: sampleStep + 1, positiveSamples: 0, decayFactor: 0 };
    }
    const lastMessageStep = firstMessageStep + (Math.floor((sampleStep - firstMessageStep) / period) * period);
    const previousMessageStep = lastMessageStep - period >= firstMessageStep ? lastMessageStep - period : -1;
    const hasMessage = lastMessageStep === sampleStep;
    const lastMessageAge = sampleStep - lastMessageStep;
    let positiveSamples = 0;
    if (hasMessage) {
      positiveSamples = previousMessageStep >= 0 && (sampleStep - previousMessageStep) <= MONITOR_STALE_AFTER_SAMPLES
        ? MONITOR_COUNTER_CACHE
        : 1;
    }
    const decayFactor = hasMessage ? 1 : lastMessageAge === 1 ? 0.45 : lastMessageAge === 2 ? 0.12 : 0;
    return { hasMessage, lastMessageAge, positiveSamples, decayFactor };
  };
  const getTransientState = (row, sampleStep) => {
    const profile = monitorTransientProfiles.get(row.url);
    if (!profile) return null;
    if (sampleStep < profile.dropAt) return null;
    if (sampleStep < profile.recoverAt) {
      const lastMessageAge = sampleStep - profile.dropAt + 1;
      const decayFactor = lastMessageAge === 1 ? 0.45 : lastMessageAge === 2 ? 0.12 : 0;
      return { hasMessage: false, lastMessageAge, positiveSamples: 0, decayFactor };
    }
    const positiveSamples = Math.min((sampleStep - profile.recoverAt) + 1, MONITOR_COUNTER_CACHE);
    return { hasMessage: true, lastMessageAge: 0, positiveSamples, decayFactor: 1 };
  };

  const createMonitorSample = (row, sampleStep) => {
    if (row.passive) {
      return {
        stateClass: 'monitor-plain',
        freq: '---',
        rate: '---',
        loss: '---',
        latency: '---',
        freqNum: 0,
        rateNum: 0,
        lossNum: 0,
        latencyNum: 0,
      };
    }
    const seed = row.url.length % 17;
    let hasMessage = true;
    let lastMessageAge = 0;
    let positiveSamples = sampleStep + 1;
    let valueFactor = 1;
    const transientState = getTransientState(row, sampleStep);
    if (transientState) {
      hasMessage = transientState.hasMessage;
      lastMessageAge = transientState.lastMessageAge;
      positiveSamples = transientState.positiveSamples;
      valueFactor = transientState.decayFactor;
    } else if (row.activity === 'stale') {
      hasMessage = false;
      lastMessageAge = sampleStep + 1;
      positiveSamples = 0;
      valueFactor = 0;
    } else if (row.baseFreq > 0 && row.baseFreq < 1) {
      const lowFreqState = getLowFreqState(row, sampleStep);
      hasMessage = lowFreqState.hasMessage;
      lastMessageAge = lowFreqState.lastMessageAge;
      positiveSamples = lowFreqState.positiveSamples;
      valueFactor = lowFreqState.decayFactor;
    }
    const warmupSamples = row.activity === 'warm' ? MONITOR_WARM_SAMPLES : MONITOR_COUNTER_CACHE;
    if (!hasMessage && lastMessageAge > MONITOR_STALE_AFTER_SAMPLES) {
      return {
        stateClass: 'monitor-err',
        freq: '0.00Hz',
        rate: '0.00B/s',
        loss: '0.00%',
        latency: '---',
        freqNum: 0,
        rateNum: 0,
        lossNum: 0,
        latencyNum: 0,
      };
    }
    const freqBase = hasMessage
      ? row.baseFreq + noise(seed, sampleStep, row.baseFreq > 5 ? row.baseFreq * 0.012 : 0.08, 3.6)
      : row.baseFreq * valueFactor;
    const rateBase = hasMessage
      ? row.baseRate + noise(seed + 3, sampleStep, Math.max(row.baseRate * 0.028, 64), 4.4)
      : row.baseRate * valueFactor;
    const lossBase = hasMessage
      ? row.baseLoss + noise(seed + 5, sampleStep, row.baseLoss > 0 ? row.baseLoss * 0.5 : 0.004, 5.2)
      : row.baseLoss * valueFactor;
    const latencyBase = row.baseLatency == null
      ? null
      : hasMessage
        ? row.baseLatency + noise(seed + 7, sampleStep, Math.max(row.baseLatency * 0.10, 0.03), 3.1)
        : (valueFactor > 0 ? row.baseLatency : null);
    const freqNum = Math.max(freqBase, 0);
    const rateNum = Math.max(rateBase, 0);
    const lossNum = Math.max(lossBase, 0);
    const latencyNum = latencyBase == null ? null : Math.max(latencyBase, 0);
    return {
      stateClass: hasMessage && positiveSamples >= warmupSamples ? 'monitor-ok' : 'monitor-warn',
      freq: formatFreq(freqNum),
      rate: formatRateSize(rateNum),
      loss: formatLoss(lossNum),
      latency: formatLatency(latencyNum),
      freqNum,
      rateNum,
      lossNum,
      latencyNum: latencyNum == null ? 0 : latencyNum,
    };
  };
  const buildMonitorHistories = (row, sampleStep, chartWidth) => {
    const freq = [];
    const rate = [];
    const loss = [];
    const latency = [];
    const start = Math.max(sampleStep - chartWidth + 1, 0);
    for (let i = start; i <= sampleStep; i += 1) {
      const sample = createMonitorSample(row, i);
      freq.push(sample.freqNum);
      rate.push(sample.rateNum);
      loss.push(sample.lossNum);
      latency.push(sample.latencyNum);
    }
    return { freq, rate, loss, latency };
  };
  const renderChartLines = (title, values, unit, chartHeight, chartWidth, unitValue) => {
    const labelWidth = 5;
    const labelGap = 1;
    const prefixWidth = labelWidth + labelGap + 1;
    const lines = [];
    const titleEnd = `(0 - ${chartWidth}s)`;
    const current = values.length ? `: ${formatChartValue(values[values.length - 1], unitValue)}${unit}` : '';
    const titleContent = `${title}${current}`;
    let titleLine = '';
    if (chartWidth < 16) {
      titleLine = fit(titleContent, chartWidth);
    } else {
      const padding = Math.max(chartWidth - titleContent.length - titleEnd.length, 0);
      titleLine = padding > 0
        ? `${titleContent}${' '.repeat(padding)}${titleEnd}`
        : `${clip(titleContent, chartWidth)}${titleEnd}`;
    }
    lines.push(`${' '.repeat(prefixWidth)}${fit(titleLine, chartWidth)}`);
    const blocks = ['▁', '▂', '▃', '▄', '▅', '▆', '▇', '█'];
    const minVal = values.length ? Math.min(...values) : 0;
    const rawMax = values.length ? Math.max(...values) : 1;
    const maxVal = rawMax <= minVal ? minVal + 1 : rawMax;
    let range = maxVal - minVal;
    let normalizedMin = minVal;
    let normalizedMax = maxVal;
    if (range < 1e-9) {
      if (normalizedMax > 0) {
        range = normalizedMax * 0.1;
        normalizedMin = Math.max(0, normalizedMax - range);
      } else {
        range = 1.0;
        normalizedMin = 0.0;
        normalizedMax = 1.0;
      }
    }
    const visible = values.length > chartWidth ? values.slice(values.length - chartWidth) : values;
    const pad = Math.max(chartWidth - visible.length, 0);
    for (let row = chartHeight - 1; row >= 0; row -= 1) {
      let label = ' '.repeat(labelWidth);
      if (row === chartHeight - 1) label = String(formatChartValue(normalizedMax, unitValue)).padStart(labelWidth, ' ');
      else if (row === 0) label = String(formatChartValue(normalizedMin, unitValue)).padStart(labelWidth, ' ');
      else if (row === Math.floor(chartHeight / 2) && chartHeight >= 5) {
        label = String(formatChartValue((normalizedMin + normalizedMax) / 2, unitValue)).padStart(labelWidth, ' ');
      }
      let spark = ' '.repeat(pad);
      visible.forEach((value) => {
        const thresholdBottom = normalizedMin + (range * row / Math.max(chartHeight, 1));
        const thresholdTop = normalizedMin + (range * (row + 1) / Math.max(chartHeight, 1));
        if (value >= thresholdTop) {
          spark += blocks[7];
        } else if (value >= thresholdBottom) {
          const levelRatio = (value - thresholdBottom) / Math.max(thresholdTop - thresholdBottom, 1e-9);
          const sparkLevel = clamp(Math.ceil(levelRatio * 7), 1, 7);
          spark += blocks[sparkLevel];
        } else {
          spark += ' ';
        }
      });
      lines.push(`${label}${' '.repeat(labelGap)}│${spark}`);
    }
    lines.push(`${' '.repeat(prefixWidth)}${'‾'.repeat(chartWidth)}`);
    return lines;
  };
  const renderRightPanel = (history, panelHeight, chartWidth) => {
    const chartLineWidth = chartWidth + 7;
    const panelLines = [];
    let numCharts = 4;
    let chartRows = Math.floor((panelHeight - (numCharts * 3)) / numCharts);
    if (panelHeight < 20) {
      numCharts = 2;
      chartRows = Math.floor((panelHeight - (numCharts * 3)) / numCharts);
    }
    if (panelHeight < 12) {
      numCharts = 1;
      chartRows = panelHeight - 3;
    }
    chartRows = clamp(chartRows, 1, 30);
    panelLines.push(' '.repeat(chartLineWidth));
    if (numCharts >= 1) {
      panelLines.push(...renderChartLines('Freq', history.freq, 'Hz', chartRows, chartWidth, 1000));
      if (numCharts > 1) panelLines.push(' '.repeat(chartLineWidth));
    }
    if (numCharts >= 2) {
      panelLines.push(...renderChartLines('Rate', history.rate, 'B/s', chartRows, chartWidth, 1024));
      if (numCharts > 2) panelLines.push(' '.repeat(chartLineWidth));
    }
    if (numCharts >= 3) {
      panelLines.push(...renderChartLines('Loss', history.loss, '%', chartRows, chartWidth, 100));
      if (numCharts > 3) panelLines.push(' '.repeat(chartLineWidth));
    }
    if (numCharts >= 4) {
      panelLines.push(...renderChartLines('Latency', history.latency, 'ms', chartRows, chartWidth, 1000));
    }
    return padScreen(panelLines, panelHeight);
  };
  const buildMonitorLayout = ({ cols, innerWidth }) => {
    const compact = innerWidth < 980;
    const typeWidth = Math.max(7, '[TYPE]'.length);
    const typeGap = compact ? 2 : 4;
    const urlGap = compact ? 2 : 3;
    const metricGap = 1;
    const freqWidth = Math.max(compact ? 8 : 10, '[FREQ]'.length);
    const rateWidth = Math.max(compact ? 10 : 11, '[RATE]'.length);
    const lossWidth = Math.max(compact ? 5 : 7, '[LOSS]'.length);
    const latencyWidth = Math.max(compact ? 6 : 9, '[LATENCY]'.length);
    const urlWidth = Math.max(compact ? 18 : 24, '[URL]'.length);
    const chartTitlePadding = 10;
    const chartContentInset = 1;
    const chartPanelExtraWidth = 8;
    const leftWidth = typeWidth + typeGap + urlWidth + urlGap + freqWidth + metricGap + rateWidth + metricGap + lossWidth + metricGap + latencyWidth;
    let chartWidth = 0;
    if (innerWidth >= 1180) chartWidth = 30;
    else if (innerWidth >= 980) chartWidth = 26;
    else if (innerWidth >= 920) chartWidth = 24;
    else if (innerWidth >= 820) chartWidth = 20;
    else if (innerWidth >= 720) chartWidth = 16;
    else if (innerWidth >= 660) chartWidth = 12;
    if (chartWidth < 10) chartWidth = 0;
    let chartPanelWidth = chartWidth > 0 ? chartWidth + chartPanelExtraWidth : 0;
    return {
      typeWidth,
      typeGap,
      urlWidth,
      urlGap,
      freqWidth,
      rateWidth,
      lossWidth,
      latencyWidth,
      metricGap,
      leftWidth,
      chartTitlePadding,
      chartContentInset,
      chartWidth,
      chartPanelWidth,
    };
  };
  const renderMonitorTitle = (widths) => {
    const leftTitle =
      fit('[TYPE]', widths.typeWidth) + ' '.repeat(widths.typeGap) +
      fit('[URL]', widths.urlWidth) + ' '.repeat(widths.urlGap) +
      fit('[FREQ]', widths.freqWidth) + ' '.repeat(widths.metricGap) +
      fit('[RATE]', widths.rateWidth) + ' '.repeat(widths.metricGap) +
      fit('[LOSS]', widths.lossWidth) + ' '.repeat(widths.metricGap) +
      fit('[LATENCY]', widths.latencyWidth);
    if (widths.chartPanelWidth > 0) {
      const rightTitle = fit(`${' '.repeat(widths.chartTitlePadding)}[CHART]`, widths.chartPanelWidth);
      return `<span class="monitor-head">${escapeTerminalText(fit(leftTitle, widths.leftWidth))}</span><span class="monitor-bar"> </span><span class="monitor-head">${escapeTerminalText(rightTitle)}</span>`;
    }
    return `<span class="monitor-head">${escapeTerminalText(fit(leftTitle, widths.leftWidth))}</span>`;
  };
  const renderMonitorLine = (row, sample, widths) => (
    fit(row.type, widths.typeWidth) + ' '.repeat(widths.typeGap) +
    fit(clip(row.url, widths.urlWidth), widths.urlWidth) + ' '.repeat(widths.urlGap) +
    fit(sample.freq, widths.freqWidth) + ' '.repeat(widths.metricGap) +
    fit(sample.rate, widths.rateWidth) + ' '.repeat(widths.metricGap) +
    fit(sample.loss, widths.lossWidth) + ' '.repeat(widths.metricGap) +
    fit(sample.latency, widths.latencyWidth)
  );
  const buildMonitorScript = (bodyRows) => {
    const downStepMs = 240;
    const pageStepMs = 480;
    const idleBeforeSelectMs = 1000;
    const enterDelayMs = 900;
    const returnPauseMs = 1000;
    const nextPagePauseMs = 1200;
    const secondPageSettleMs = 240;
    const secondSelectionHoldMs = 3000;
    const eproto1Duration = 250 + 5000;
    const eproto2Duration = 250 + 4200;
    const firstOffset = firstEprotoIndex;
    const secondPage = Math.floor(secondEprotoIndex / bodyRows);
    const secondOffset = secondEprotoIndex - (secondPage * bodyRows);
    const actions = [];
    let cursor = idleBeforeSelectMs;
    for (let i = 0; i <= firstOffset; i += 1) {
      actions.push({ at: cursor, key: 'down' });
      cursor += downStepMs;
    }
    const enter1At = cursor + enterDelayMs;
    cursor = enter1At + eproto1Duration + returnPauseMs;
    for (let i = 0; i < secondPage; i += 1) {
      actions.push({ at: cursor, key: 'right' });
      cursor += pageStepMs;
    }
    cursor += nextPagePauseMs + secondPageSettleMs;
    for (let i = 0; i <= secondOffset; i += 1) {
      actions.push({ at: cursor, key: 'down' });
      cursor += downStepMs;
    }
    const enter2At = cursor + secondSelectionHoldMs;
    cursor = enter2At + eproto2Duration + returnPauseMs;
    actions.push({ at: cursor, key: 'right' });
    cursor += 420;
    for (let i = 0; i < 4; i += 1) {
      actions.push({ at: cursor, key: 'down' });
      cursor += 220;
    }
    const cycleMs = cursor + 1600;
    return {
      actions,
      enter1At,
      enter2At,
      eproto1Duration,
      eproto2Duration,
      activeWindows: [
        [0, enter1At],
        [enter1At + eproto1Duration, enter2At],
        [enter2At + eproto2Duration, cycleMs],
      ],
      cycleMs,
    };
  };
  const applyMonitorKey = (state, key, bodyRows, rowCount) => {
    const totalPages = Math.max(Math.ceil(rowCount / bodyRows), 1);
    if (key === 'right') {
      if (state.currentPage < totalPages - 1) {
        state.currentPage += 1;
        state.selectedIndex = -1;
      }
      return;
    }
    if (key === 'left') {
      if (state.currentPage > 0) {
        state.currentPage -= 1;
        state.selectedIndex = -1;
      }
      return;
    }
    const startIndex = state.currentPage * bodyRows;
    const endIndex = Math.min(startIndex + bodyRows, rowCount);
    if (key === 'up') {
      if (state.selectedIndex < 0) {
        state.selectedIndex = clamp(((state.currentPage + 1) * bodyRows) - 1, 0, rowCount - 1);
      } else if (state.selectedIndex === startIndex && state.currentPage > 0) {
        state.currentPage -= 1;
        const newStart = state.currentPage * bodyRows;
        const newEnd = Math.min(newStart + bodyRows, rowCount);
        state.selectedIndex = newEnd - 1;
      } else {
        state.selectedIndex = Math.max(state.selectedIndex - 1, 0);
      }
      return;
    }
    if (key === 'down') {
      if (state.selectedIndex < 0) {
        state.selectedIndex = startIndex;
      } else if (state.selectedIndex === endIndex - 1 && state.currentPage < totalPages - 1) {
        state.currentPage += 1;
        state.selectedIndex = state.currentPage * bodyRows;
      } else {
        state.selectedIndex = Math.min(state.selectedIndex + 1, rowCount - 1);
      }
    }
  };
  const simulateMonitorState = (actions, elapsedMs, bodyRows, rowCount) => {
    const state = { currentPage: 0, selectedIndex: -1 };
    actions.forEach((action) => {
      if (action.at <= elapsedMs) applyMonitorKey(state, action.key, bodyRows, rowCount);
    });
    return state;
  };
  const accumulateWindowMs = (windows, elapsedMs) => windows.reduce((sum, [start, end]) => {
    if (elapsedMs <= start) return sum;
    return sum + Math.max(0, Math.min(elapsedMs, end) - start);
  }, 0);

  const buildPointCloudLines = (fastStep) => {
    const seq = 9800 + fastStep;
    const timeMeas = formatUtcDate(Date.UTC(2026, 3, 17, 6, 43, 10, 80) + (fastStep * 100));
    const timePub = formatUtcDate(Date.UTC(2026, 3, 17, 6, 43, 10, 82) + (fastStep * 100));
    const lines = [
      'header {',
      '  frame_id: "front_lidar"',
      `  seq: ${seq}`,
      `  time_meas: "${timeMeas}"`,
      `  time_pub: "${timePub}"`,
      '}',
      'protocol {',
      '  size_list: 4',
      '  size_list: 4',
      '  size_list: 4',
      '  size_list: 2',
      '  size_list: 2',
      '  size_list: 8',
      '  name_list: "x"',
      '  name_list: "y"',
      '  name_list: "z"',
      '  name_list: "intensity"',
      '  name_list: "ring"',
      '  name_list: "timestamp_us"',
      '  type_list: "float"',
      '  type_list: "float"',
      '  type_list: "float"',
      '  type_list: "uint16"',
      '  type_list: "uint16"',
      '  type_list: "uint64"',
      '}',
      `size: ${1572864 + ((fastStep % 4) * 4096)}`,
      'pack_size: 131072',
    ];
    for (let i = 0; i < 12; i += 1) {
      const base = fastStep + i;
      lines.push(`data[${i}] {`);
      lines.push(`  x: ${(24.10 + noise(i + 1, base, 0.85, 2.4)).toFixed(6)}`);
      lines.push(`  y: ${(-2.40 + noise(i + 3, base, 0.60, 2.2)).toFixed(6)}`);
      lines.push(`  z: ${(0.15 + noise(i + 5, base, 0.22, 2.6)).toFixed(6)}`);
      lines.push(`  intensity: ${Math.round(48 + noise(i + 7, base, 8, 2.1))}`);
      lines.push(`  ring: ${16 + (i % 8)}`);
      lines.push(`  timestamp_us: ${1713336190080000 + (fastStep * 100000) + (i * 16667)}`);
      lines.push('}');
    }
    return lines;
  };
  const buildGnssLines = (fastStep) => {
    const seq = 4210 + fastStep;
    const timeMeas = formatUtcDate(Date.UTC(2026, 3, 17, 6, 43, 26, 120) + (fastStep * 100));
    const timePub = formatUtcDate(Date.UTC(2026, 3, 17, 6, 43, 26, 122) + (fastStep * 100));
    const latitude = 31.231703 + noise(1, fastStep, 0.000006, 3.2);
    const longitude = 121.472644 + noise(2, fastStep, 0.000006, 3.0);
    const altitude = 7.420 + noise(3, fastStep, 0.12, 2.8);
    const speed = 13.850 + noise(4, fastStep, 0.05, 2.5);
    const lines = [
      'header {',
      '  frame_id: "gnss_fix"',
      `  seq: ${seq}`,
      `  time_meas: "${timeMeas}"`,
      `  time_pub: "${timePub}"`,
      '}',
      'gnss {',
      '  status: FIXED_RTK',
      '  position_type: 50',
      `  latitude_deg: ${latitude.toFixed(9)}`,
      `  longitude_deg: ${longitude.toFixed(9)}`,
      `  altitude_m: ${altitude.toFixed(4)}`,
      `  speed_mps: ${speed.toFixed(4)}`,
      `  heading_deg: ${(184.20 + noise(5, fastStep, 0.30, 2.4)).toFixed(4)}`,
      `  age_ms: ${(84.0 + noise(6, fastStep, 8.0, 2.8)).toFixed(3)}`,
      '  mode: 4',
      '  valid: true',
      '}',
      'covariance {',
      `  xx: ${(0.0120 + noise(7, fastStep, 0.0012, 3.1)).toFixed(6)}`,
      `  yy: ${(0.0116 + noise(8, fastStep, 0.0012, 3.0)).toFixed(6)}`,
      `  zz: ${(0.0202 + noise(9, fastStep, 0.0018, 2.9)).toFixed(6)}`,
      `  xy: ${(0.0002 + noise(10, fastStep, 0.0001, 2.8)).toFixed(6)}`,
      `  xz: ${(0.0001 + noise(11, fastStep, 0.0001, 2.7)).toFixed(6)}`,
      `  yz: ${(0.0001 + noise(12, fastStep, 0.0001, 2.6)).toFixed(6)}`,
      '}',
      'velocity_ned {',
      `  north_mps: ${(-13.80 + noise(13, fastStep, 0.08, 2.4)).toFixed(6)}`,
      `  east_mps: ${(0.62 + noise(14, fastStep, 0.03, 2.3)).toFixed(6)}`,
      `  down_mps: ${(-0.02 + noise(15, fastStep, 0.01, 2.2)).toFixed(6)}`,
      '}',
    ];
    for (let i = 0; i < 10; i += 1) {
      lines.push(`satellite[${i}] {`);
      lines.push(`  prn: ${5 + i}`);
      lines.push(`  snr: ${(39.0 + noise(i + 16, fastStep, 3.0, 2.5)).toFixed(3)}`);
      lines.push(`  elevation_deg: ${(22.0 + i * 4.2 + noise(i + 26, fastStep, 0.8, 2.2)).toFixed(3)}`);
      lines.push(`  azimuth_deg: ${(35.0 + i * 29.0 + noise(i + 36, fastStep, 1.3, 2.1)).toFixed(3)}`);
      lines.push(`  used: ${i < 7 ? 'true' : 'false'}`);
      lines.push('}');
    }
    return lines;
  };
  const renderEprotoScreen = (metrics, phaseTimeMs, variant) => {
    const { rows, cols } = metrics;
    const bodyRows = Math.max(rows - 3, 3);
    const url = variant === 'pointcloud' ? firstEprotoTargetUrl : secondEprotoTargetUrl;
    if (phaseTimeMs < 250) {
      return composeScreen(
        [
          'Message Parsed by vlink-eproto (Wait For Message)...',
          `<span class="url">${escapeTerminalText(url)}</span>`,
        ],
        [],
        '',
        rows,
      );
    }
    const liveMs = phaseTimeMs - 250;
    const sampleStep = Math.floor(liveMs / 1000);
    const fastStep = Math.floor(liveMs / 100);
    const rate = variant === 'pointcloud'
      ? Math.max(9.90 + noise(2, sampleStep, 0.16, 2.8), 0)
      : Math.max(10.00 + noise(4, sampleStep, 0.12, 2.6), 0);
    const rateClass = sampleStep < 2 ? 'warn' : 'ok';
    const bodyLines = (variant === 'pointcloud' ? buildPointCloudLines(fastStep) : buildGnssLines(fastStep))
      .map((line) => `<span class="ok">${escapeTerminalText(clip(line, cols))}</span>`);
    const pages = chunkPages(bodyLines, bodyRows);
    const totalPages = pages.length;
    const page = totalPages <= 1 ? 0 : Math.min(totalPages - 1, Math.floor(liveMs / 2200));
    const footer = `<span class="monitor-head">&lt;${page + 1}/${totalPages}&gt;</span> [ ${footerToggle('E', true)} ${footerToggle('R', false)} ${footerToggle('T', false)} ${footerToggle('Y', true)} ${footerToggle('U', false)} ${footerToggle('O', false)} ${footerToggle('P', false)} ]`;
    return composeScreen(
      [
        'Message Parsed by vlink-eproto:',
        `<span class="url">${escapeTerminalText(url)}</span> <span class="${rateClass}">&#9679; ${formatFreq(rate)}</span>`,
      ],
      pages[page],
      footer,
      rows,
    );
  };

  const renderMonitorScreen = (metrics, phaseTimeMs) => {
    const { rows, cols } = metrics;
    const cmd = 'vlink-monitor -loc';
    const typingMs = commandTypingDuration(cmd);
    const holdMs = 220;
    const waitMs = 1000;
    const elapsed = phaseTimeMs;
    if (elapsed < typingMs) return padScreen([typedPromptLine(cmd, elapsed)], rows);
    if (elapsed < typingMs + holdMs) return padScreen([prompt(cmd)], rows);
    if (elapsed < typingMs + holdMs + waitMs) {
      return padScreen([prompt(cmd), '', 'Information Collecting, Please Wait...'], rows);
    }
    const bodyRows = Math.max(rows - 3, 3);
    const script = buildMonitorScript(bodyRows);
    const liveElapsed = (elapsed - typingMs - holdMs - waitMs) % script.cycleMs;
    if (liveElapsed >= script.enter1At && liveElapsed < script.enter1At + script.eproto1Duration) {
      return renderEprotoScreen(metrics, liveElapsed - script.enter1At, 'pointcloud');
    }
    if (liveElapsed >= script.enter2At && liveElapsed < script.enter2At + script.eproto2Duration) {
      return renderEprotoScreen(metrics, liveElapsed - script.enter2At, 'gnss');
    }
    const widths = buildMonitorLayout(metrics);
    const sampleStep = Math.floor(accumulateWindowMs(script.activeWindows, liveElapsed) / 1000);
    const state = simulateMonitorState(script.actions, liveElapsed, bodyRows, monitorRows.length);
    const sampledRows = monitorRows.map((row) => ({ row, sample: createMonitorSample(row, sampleStep) }));
    const activeCount = sampledRows.filter(({ sample }) => sample.stateClass === 'monitor-ok').length;
    const totalRate = sampledRows.reduce((sum, { sample }) => sum + sample.rateNum, 0);
    const pageCount = Math.max(Math.ceil(sampledRows.length / bodyRows), 1);
    const currentPage = clamp(state.currentPage, 0, pageCount - 1);
    const startIndex = currentPage * bodyRows;
    const endIndex = Math.min(startIndex + bodyRows, sampledRows.length);
    const hasCurrentSelection = state.selectedIndex >= startIndex && state.selectedIndex < endIndex;
    const selectedRow = hasCurrentSelection ? sampledRows[state.selectedIndex].row : null;
    const history = widths.chartWidth > 0 && selectedRow
      ? buildMonitorHistories(selectedRow, sampleStep, widths.chartWidth)
      : { freq: [], rate: [], loss: [], latency: [] };
    const chartPanel = widths.chartWidth > 0 ? renderRightPanel(history, bodyRows, widths.chartWidth) : [];
    const body = [];
    for (let i = startIndex; i < endIndex; i += 1) {
      const { row, sample } = sampledRows[i];
      const left = fit(renderMonitorLine(row, sample, widths), widths.leftWidth);
      const chart = widths.chartPanelWidth > 0
        ? `${' '.repeat(widths.chartContentInset)}${(chartPanel[i - startIndex] || ' '.repeat(widths.chartWidth + 7))}`
        : '';
      if (i === state.selectedIndex) {
        body.push(`<span class="monitor-sel">${escapeTerminalText(left)}</span>${widths.chartPanelWidth > 0 ? `<span class="monitor-bar"> </span>${escapeTerminalText(chart)}` : ''}`);
      } else {
        body.push(`<span class="${sample.stateClass}">${escapeTerminalText(left)}</span>${widths.chartPanelWidth > 0 ? `<span class="monitor-bar"> </span>${escapeTerminalText(chart)}` : ''}`);
      }
    }
    for (let i = endIndex - startIndex; i < bodyRows; i += 1) {
      const left = ' '.repeat(widths.leftWidth);
      const chart = widths.chartPanelWidth > 0
        ? `${' '.repeat(widths.chartContentInset)}${(chartPanel[i] || ' '.repeat(widths.chartWidth + 7))}`
        : '';
      body.push(`${escapeTerminalText(left)}${widths.chartPanelWidth > 0 ? `<span class="monitor-bar"> </span>${escapeTerminalText(chart)}` : ''}`);
    }
    const footer = `<span class="monitor-head">&lt;${currentPage + 1}/${pageCount}&gt;</span> [ ${footerToggle('T', false)} ${footerToggle('L', true)} ${footerToggle('O', true)} ${footerToggle('E', false)} ${footerToggle('S', false)} ${footerToggle('A', false)} ${footerToggle('Y', false)} ${footerToggle('P', false)} ${footerToggle('C', true)} ] | Total: ${sampledRows.length} | Active: ${activeCount} | Rate: ${formatRateSize(totalRate)}`;
    return composeScreen(
      ['Information Collected by vlink-monitor:', renderMonitorTitle(widths)],
      body,
      footer,
      rows,
    );
  };

  const renderListScreen = (metrics, phaseTimeMs) => {
    const { rows } = metrics;
    const cmd = 'vlink-list';
    const typingMs = commandTypingDuration(cmd);
    const holdMs = 220;
    const waitMs = 1000;
    const viewMs = 2600;
    const exitMs = 1200;
    const cycleMs = typingMs + holdMs + waitMs + viewMs + exitMs;
    const elapsed = phaseTimeMs % cycleMs;
    if (elapsed < typingMs) return padScreen([typedPromptLine(cmd, elapsed)], rows);
    if (elapsed < typingMs + holdMs) return padScreen([prompt(cmd)], rows);
    if (elapsed < typingMs + holdMs + waitMs) {
      return padScreen([prompt(cmd), '', 'Information Collecting, Please Wait...'], rows);
    }
    const maxUrl = Math.max(...listProcesses.flatMap((process) =>
      ['server', 'client', 'publisher', 'subscriber', 'setter', 'getter']
        .flatMap((key) => (process[key] || []).map(([url]) => url.length))
    ), 10);
    const lines = [prompt(cmd), ''];
    listProcesses.forEach((process) => {
      lines.push(`${process.name} (pid: <span class="num">${process.pid}</span>, host: ${process.host}, ip: <span class="url">${process.ip}</span>)`);
      [['server', 'Server'], ['client', 'Client'], ['publisher', 'Publisher'], ['subscriber', 'Subscriber'], ['setter', 'Setter'], ['getter', 'Getter']].forEach(([key, label]) => {
        if (!(process[key] || []).length) return;
        lines.push(`  ${label}:`);
        process[key].forEach(([url, ser]) => {
          lines.push(`    <span class="url">${escapeTerminalText(url)}</span>${' '.repeat(Math.max(maxUrl - url.length + 4, 4))}<span class="dim">${escapeTerminalText(ser)}</span>`);
        });
      });
      lines.push('');
    });
    if (elapsed < typingMs + holdMs + waitMs + viewMs) return padScreen(lines, rows);
    return withExitPrompt(lines, rows);
  };

  const renderCheckDiagLines = (finishedCount, currentItem) => {
    const lines = [
      prompt('vlink-check diag'),
      '',
      '<span class="hi">[TITLE]                                          [STATUS]                                         [DETAIL]</span>',
    ];
    checkDiagItems.slice(0, finishedCount).forEach(([title, level, detail]) => {
      const statusText = level === 'ok'
        ? '<span class="ok">PASSED</span>'
        : level === 'warn'
          ? '<span class="warn">WARNING</span>'
          : '<span class="err">FAILED</span>';
      const statusPadding = level === 'warn' ? Math.max(49 - detail.length, 2) : Math.max(50 - detail.length, 2);
      lines.push(`${escapeTerminalText(title)}${' '.repeat(Math.max(50 - title.length, 0))}${statusText}${' '.repeat(statusPadding)}${escapeTerminalText(detail)}`);
    });
    if (currentItem) {
      lines.push(`${escapeTerminalText(currentItem[0])}${' '.repeat(Math.max(50 - currentItem[0].length, 0))}......`);
    }
    return lines;
  };
  const renderCheckScreen = (metrics, phaseTimeMs) => {
    const { rows } = metrics;
    const diagCmd = 'vlink-check diag';
    const envCmd = 'vlink-check env';
    const diagTypingMs = commandTypingDuration(diagCmd);
    const envTypingMs = commandTypingDuration(envCmd);
    const promptHoldMs = 220;
    const exitMs = 1200;
    const envHoldMs = 1800;
    const diagTotalMs = checkDiagItems.reduce((sum, item) => sum + item[3], 0);
    const cycleMs = diagTypingMs + promptHoldMs + diagTotalMs + exitMs + envTypingMs + promptHoldMs + envHoldMs + exitMs;
    const elapsed = phaseTimeMs % cycleMs;
    if (elapsed < diagTypingMs) return padScreen([typedPromptLine(diagCmd, elapsed)], rows);
    if (elapsed < diagTypingMs + promptHoldMs) return padScreen([prompt(diagCmd)], rows);
    const diagElapsed = elapsed - diagTypingMs - promptHoldMs;
    if (diagElapsed < diagTotalMs) {
      let acc = 0;
      let finished = 0;
      let current = null;
      for (let i = 0; i < checkDiagItems.length; i += 1) {
        const item = checkDiagItems[i];
        if (diagElapsed >= acc + item[3]) {
          finished += 1;
          acc += item[3];
        } else {
          current = item;
          break;
        }
      }
      return padScreen(renderCheckDiagLines(finished, current), rows);
    }
    const finalDiagLines = renderCheckDiagLines(checkDiagItems.length, null);
    if (diagElapsed < diagTotalMs + exitMs) {
      return withExitPrompt(finalDiagLines, rows);
    }
    const envElapsed = diagElapsed - diagTotalMs - exitMs;
    if (envElapsed < envTypingMs) return padScreen([typedPromptLine(envCmd, envElapsed)], rows);
    if (envElapsed < envTypingMs + promptHoldMs) return padScreen([prompt(envCmd)], rows);
    const lines = [prompt(envCmd), ''];
    checkEnvItems.forEach(([key, value, desc, isSet]) => {
      if (isSet) lines.push(`<span class="ok">[${key}]: ${escapeTerminalText(value)}</span>`);
      else lines.push(`<span class="err">[${key}]</span>`);
      lines.push(escapeTerminalText(desc));
      lines.push('');
    });
    if (envElapsed < envTypingMs + promptHoldMs + envHoldMs) return padScreen(lines, rows);
    return withExitPrompt(lines, rows);
  };

  const renderBagInfoTable = () => {
    const maxType = Math.max(...bagInfoMetas.map((row) => row[0].length), 6);
    const maxCount = Math.max(...bagInfoMetas.map((row) => row[1].length), 7);
    const maxSize = Math.max(...bagInfoMetas.map((row) => row[2].length), 7);
    const maxFreq = Math.max(...bagInfoMetas.map((row) => row[3].length), 7);
    const maxLoss = Math.max(...bagInfoMetas.map((row) => row[4].length), 6);
    const maxUrl = Math.max(...bagInfoMetas.map((row) => row[5].length), 10);
    const padColumn = (width, textLength) => ' '.repeat(Math.max(width - textLength + 2, 2));
    const lines = [`Meta List:     [Type]${' '.repeat(Math.max(maxType - 4, 0))}[Count]${' '.repeat(Math.max(maxCount - 5, 0))}[Size]${' '.repeat(Math.max(maxSize - 4, 0))}[Freq]${' '.repeat(Math.max(maxFreq - 4, 0))}[Loss]${' '.repeat(Math.max(maxLoss - 4, 0))}[Url]${' '.repeat(Math.max(maxUrl - 3, 0))}[Ser]`];
    bagInfoMetas.forEach(([type, count, size, freq, loss, url, ser]) => {
      lines.push(
        `               ${escapeTerminalText(type)}${padColumn(maxType, type.length)}` +
        `<span class="num">${escapeTerminalText(count)}</span>${padColumn(maxCount, count.length)}` +
        `<span class="num">${escapeTerminalText(size)}</span>${padColumn(maxSize, size.length)}` +
        `<span class="num">${escapeTerminalText(freq)}</span>${padColumn(maxFreq, freq.length)}` +
        `<span class="num">${escapeTerminalText(loss)}</span>${padColumn(maxLoss, loss.length)}` +
        `<span class="url">${escapeTerminalText(url)}</span>${padColumn(maxUrl, url.length)}` +
        `<span class="dim">${escapeTerminalText(ser)}</span>`
      );
    });
    return lines;
  };
  const renderBagInfoStage = (rows, elapsed) => {
    const cmd = 'vlink-bag info /data/drive.vdb';
    const typingMs = commandTypingDuration(cmd);
    const holdMs = 220;
    if (elapsed < typingMs) return { done: false, lines: padScreen([typedPromptLine(cmd, elapsed)], rows) };
    if (elapsed < typingMs + holdMs) return { done: false, lines: padScreen([prompt(cmd)], rows) };
    const lines = [
      prompt(cmd),
      '',
      'File Name:     <span class="url">/data/drive.vdb</span>',
      'File Size:     <span class="num">2.80GB</span> (Raw: <span class="num">4.12GB</span>)',
      'Tag Name:      drive-2026-04-17',
      'Version:       2.0.0',
      'Storage Type:  sqlite',
      'Compression:   lzav',
      'Process Name:  recorder_main',
      'Meta Flags:    completed | idx_elapsed | idx_url | schema',
      'Date Time:     2026-04-17 14:43:00.000 (Timezone: +08:00:00)',
      'Duration:      00:00:00:001 ~ 00:05:12:841',
      'Message Count: 2400158',
      'Split Count:   3 (By time: 120.00s)',
      ...renderBagInfoTable(),
    ];
    const stageMs = typingMs + holdMs + 3200;
    return { done: elapsed >= stageMs, lines: padScreen(lines, rows), duration: stageMs };
  };
  const renderBagRecordStage = (rows, elapsed) => {
    const cmd = 'vlink-bag record /data/drive.vdb';
    const typingMs = commandTypingDuration(cmd);
    const holdMs = 220;
    const waitMs = 1000;
    const liveMs = 3800;
    if (elapsed < typingMs) return { done: false, lines: padScreen([typedPromptLine(cmd, elapsed)], rows) };
    if (elapsed < typingMs + holdMs) return { done: false, lines: padScreen([prompt(cmd)], rows) };
    if (elapsed < typingMs + holdMs + waitMs) {
      return { done: false, lines: padScreen([prompt(cmd), '', 'Information Collecting, Please Wait...'], rows) };
    }
    const local = elapsed - typingMs - holdMs - waitMs;
    if (local < liveMs) {
      const elapsedSec = local / 1000;
      const fastStep = Math.floor(local / 50);
      const rate = Math.max(0, 91.5 * MB + noise(3, fastStep, 5.4 * MB, 12));
      const line = `<span class="ok">${formatClock(elapsedSec)} | ${formatElapsed(elapsedSec)} | ${formatRateSize(rate)} </span><span class="hi">&lt;&lt;</span><span class="ok">:</span>`;
      return { done: false, lines: padScreen([prompt(cmd), '', line], rows) };
    }
    const lines = [prompt(cmd), ''];
    const totalMs = typingMs + holdMs + waitMs + liveMs;
    return { done: elapsed >= totalMs, lines: padScreen(lines, rows), duration: totalMs };
  };
  const renderBagPlayStage = (rows, elapsed) => {
    const cmd = 'vlink-bag play /data/drive.vdb';
    const typingMs = commandTypingDuration(cmd);
    const holdMs = 220;
    const waitMs = 900;
    const liveMs = 5600;
    if (elapsed < typingMs) return { done: false, lines: padScreen([typedPromptLine(cmd, elapsed)], rows) };
    if (elapsed < typingMs + holdMs) return { done: false, lines: padScreen([prompt(cmd)], rows) };
    if (elapsed < typingMs + holdMs + waitMs) {
      return { done: false, lines: padScreen([prompt(cmd), '', 'Please Wait...'], rows) };
    }
    const local = elapsed - typingMs - holdMs - waitMs;
    const pauseStartMs = 3000;
    const pauseEndMs = 4300;
    const jumpMs = 5000;
    let elapsedSec;
    if (local < pauseStartMs) {
      elapsedSec = local / 1000;
    } else if (local < pauseEndMs) {
      elapsedSec = pauseStartMs / 1000;
    } else {
      elapsedSec = (local - pauseEndMs) / 1000 + (pauseStartMs / 1000) + 5.0;
    }
    if (local >= jumpMs) elapsedSec += 4.0;
    elapsedSec = Math.min(elapsedSec, 312.84);
    const paused = local >= pauseStartMs && local < pauseEndMs;
    const fastStep = Math.floor(local / 50);
    const rate = paused ? 0 : Math.max(0, 121.8 * MB + noise(8, fastStep, 4.8 * MB, 11));
    const percent = clamp((elapsedSec / 312.84) * 100, 0, 100);
    const line = `<span class="${paused ? 'warn' : 'ok'}">${formatClock(elapsedSec)}/00:05:12 | ${formatElapsed(elapsedSec)} | ${formatRateSize(rate)} | 1/3 </span><span class="hi">${paused ? ' || ' : ' >> '}${formatFixed(percent, 1)}% </span><span class="ok">:</span>`;
    const totalMs = typingMs + holdMs + waitMs + liveMs;
    return { done: elapsed >= totalMs, lines: padScreen([prompt(cmd), '', line], rows), duration: totalMs };
  };
  const renderBagCloneStage = (rows, elapsed) => {
    const cmd = 'vlink-bag clone /data/drive.vdb /data/drive-copy.vdb';
    const typingMs = commandTypingDuration(cmd);
    const holdMs = 220;
    const waitMs = 900;
    const progressMs = 2400;
    const doneHoldMs = 1200;
    if (elapsed < typingMs) return { done: false, lines: padScreen([typedPromptLine(cmd, elapsed)], rows) };
    if (elapsed < typingMs + holdMs) return { done: false, lines: padScreen([prompt(cmd)], rows) };
    if (elapsed < typingMs + holdMs + waitMs) {
      return { done: false, lines: padScreen([prompt(cmd), '', 'Please Wait...'], rows) };
    }
    const local = elapsed - typingMs - holdMs - waitMs;
    const progress = clamp(local / progressMs, 0, 1);
    const lines = [prompt(cmd), '', formatProgressBar(progress)];
    if (progress >= 1) lines.push('Done.');
    const totalMs = typingMs + holdMs + waitMs + progressMs + doneHoldMs;
    return { done: elapsed >= totalMs, lines: padScreen(lines, rows), duration: totalMs };
  };
  const renderBagScreen = (metrics, phaseTimeMs) => {
    const { rows } = metrics;
    const gapMs = 1200;
    const cycleMs =
      (commandTypingDuration('vlink-bag info /data/drive.vdb') + 220 + 3200) + gapMs +
      (commandTypingDuration('vlink-bag record /data/drive.vdb') + 220 + 1000 + 3800) + gapMs +
      (commandTypingDuration('vlink-bag play /data/drive.vdb') + 220 + 900 + 5600) + gapMs +
      (commandTypingDuration('vlink-bag clone /data/drive.vdb /data/drive-copy.vdb') + 220 + 900 + 2400 + 1200) + gapMs;
    let elapsed = phaseTimeMs % cycleMs;
    const infoStage = renderBagInfoStage(rows, elapsed);
    if (!infoStage.done) return infoStage.lines;
    elapsed -= infoStage.duration;
    if (elapsed < gapMs) return withExitPrompt(trimScreenPadding(infoStage.lines), rows);
    elapsed -= gapMs;
    const recordStage = renderBagRecordStage(rows, elapsed);
    if (!recordStage.done) return recordStage.lines;
    elapsed -= recordStage.duration;
    if (elapsed < gapMs) return withExitPrompt(trimScreenPadding(recordStage.lines), rows);
    elapsed -= gapMs;
    const playStage = renderBagPlayStage(rows, elapsed);
    if (!playStage.done) return playStage.lines;
    elapsed -= playStage.duration;
    if (elapsed < gapMs) return withExitPrompt(trimScreenPadding(playStage.lines), rows);
    elapsed -= gapMs;
    const cloneStage = renderBagCloneStage(rows, elapsed);
    if (!cloneStage.done) return cloneStage.lines;
    elapsed -= cloneStage.duration;
    return elapsed < gapMs ? withExitPrompt(trimScreenPadding(cloneStage.lines), rows) : cloneStage.lines;
  };

  const simulators = {
    monitor: renderMonitorScreen,
    list: renderListScreen,
    check: renderCheckScreen,
    bag: renderBagScreen,
  };
  const panelLoops = new Map();
  const panelStartTimes = new Map();
  const renderPanel = (bodyEl, key, now) => {
    const simulator = simulators[key];
    if (!simulator) return;
    if (!panelStartTimes.has(bodyEl)) panelStartTimes.set(bodyEl, now);
    const startedAt = panelStartTimes.get(bodyEl);
    const lines = simulator(measureTerminal(bodyEl), now - startedAt);
    bodyEl.innerHTML = lines.join('\n');
    bodyEl.scrollTop = 0;
  };
  const startPanelLoop = (bodyEl) => {
    const key = bodyEl.dataset.cliPanel;
    const tick = () => {
      renderPanel(bodyEl, key, performance.now());
      const interval = key === 'monitor' || key === 'bag' ? 50 : key === 'check' ? 80 : 120;
      const timer = window.setTimeout(tick, interval);
      panelLoops.set(bodyEl, timer);
    };
    tick();
  };
  panelBodies.forEach((bodyEl) => startPanelLoop(bodyEl));
})();

/* ---------- 5. QQ group modal ---------- */
(() => {
  const btn   = document.getElementById('qq-btn');
  const modal = document.getElementById('qq-modal');
  if (!btn || !modal) return;

  const open  = () => { modal.setAttribute('data-open', ''); modal.hidden = false; };
  const close = () => { modal.removeAttribute('data-open'); modal.hidden = true; };

  btn.addEventListener('click', open);
  modal.querySelector('.qq-modal-close')?.addEventListener('click', close);
  modal.addEventListener('click', (e) => { if (e.target === modal) close(); });
  document.addEventListener('keydown', (e) => {
    if (e.key === 'Escape' && !modal.hidden) close();
  });

  modal.querySelectorAll('.qq-copy').forEach((b) => {
    b.addEventListener('click', () => {
      const el = document.getElementById(b.dataset.target);
      if (!el) return;
      const text = el.textContent.replace(/\u00a0/g, '').replace(/\s+/g, '');
      const done = () => {
        const old = b.textContent;
        b.classList.add('done');
        b.textContent = '✓';
        setTimeout(() => { b.classList.remove('done'); b.textContent = old; }, 1400);
      };
      if (navigator.clipboard && navigator.clipboard.writeText) {
        navigator.clipboard.writeText(text).then(done, () => {});
      } else {
        const ta = document.createElement('textarea');
        ta.value = text; document.body.appendChild(ta); ta.select();
        try { document.execCommand('copy'); done(); } catch (_) {}
        document.body.removeChild(ta);
      }
    });
  });
})();

/* ---------- 6. Smooth scroll for hash links with offset ---------- */
document.querySelectorAll('a[href^="#"]').forEach(a => {
  a.addEventListener('click', (e) => {
    const target = document.querySelector(a.getAttribute('href'));
    if (target) {
      e.preventDefault();
      const y = target.getBoundingClientRect().top + window.scrollY - 64;
      window.scrollTo({ top: y, behavior: 'smooth' });
    }
  });
});
