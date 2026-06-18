/*
 * Copyright (C) 2026 by Thun Lu. All rights reserved.
 * Author: Thun Lu <thun.lu@zohomail.cn>
 * Repo:   https://github.com/thun-res/vlink
 *  _    __   __      _           __
 * | |  / /  / /     (_) ____    / /__
 * | | / /  / /     / / / __ \  / //_/
 * | |/ /  / /___  / / / / / / / ,<
 * |___/  /_____/ /_/ /_/ /_/ /_/|_|
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <string_view>

namespace vlink::bench::report {

inline constexpr std::string_view kBaseCss =
    R"CSS(
:root{
  --bg:#ecebe6;
  --surface:#fefefc;
  --surface-muted:#f1f0eb;
  --surface-elev:#ffffff;
  --border:rgba(30,32,40,.12);
  --border-strong:rgba(30,32,40,.24);
  --text:#1a1c22;
  --text-muted:#454852;
  --text-subtle:#5e6270;
  --text-emphasis:#0d0f15;
  --link:#3a3f86;
  --brand-700:#2a2570;
  --brand-600:#3b358f;
  --brand-500:#4d479e;
  --brand-400:#6e69b5;
  --brand-300:#9b97c9;
  --accent:#3b358f;
  --accent-hover:#2a2570;
  --accent-soft:rgba(59,53,143,.10);
  --accent-2:#1f6c87;
  --accent-3:#2f7c4f;
  --ok:#2f6c44;
  --warn:#8a4f1c;
  --fail:#8c2727;
  --ok-fg:#234a32;
  --warn-fg:#5d3210;
  --fail-fg:#5e1a1a;
  --ok-bg:rgba(47,108,68,.16);
  --warn-bg:rgba(138,79,28,.14);
  --fail-bg:rgba(140,39,39,.12);
  --chip-bg:#e3e2dc;
  --chip-fg:#1a1c22;
  --chip-border:rgba(59,53,143,.22);
  --heat-strong:#2f6c44;
  --heat-mid:#8a4f1c;
  --heat-weak:#8c2727;
  --heat-opacity:.40;
  --ok-soft:var(--ok-bg);
  --warn-soft:var(--warn-bg);
  --fail-soft:var(--fail-bg);
  --code-bg:#e7e6e0;
  --row-zebra:rgba(59,53,143,.045);
  --brand-gradient:linear-gradient(135deg,#3b358f 0%,#1f6c87 55%,#2f7c4f 100%);
  --shadow-sm:0 1px 2px rgba(20,25,40,.08);
  --shadow-md:0 4px 12px rgba(20,25,40,.08),0 0 0 1px var(--border);
  --shadow-lg:0 16px 36px rgba(20,25,40,.12),0 0 0 1px var(--border);
  --shadow:var(--shadow-sm);
  --radius-sm:8px;--radius-md:12px;--radius-lg:18px;
  --grid-line:rgba(30,32,40,.10);
  --halo:radial-gradient(1100px 380px at 12% -20%,rgba(59,53,143,.05),transparent 60%),radial-gradient(720px 260px at 92% -30%,rgba(31,108,135,.04),transparent 60%);
}
)CSS"
    R"CSS(
*,*::before,*::after{box-sizing:border-box}
html{scroll-behavior:smooth;max-width:100%;font-size:16px}
body{font-family:ui-sans-serif,system-ui,-apple-system,'Segoe UI','Inter','PingFang SC','Noto Sans SC',Roboto,sans-serif;color:var(--text);margin:0;padding:0;background:var(--bg);line-height:1.6;font-size:15px;max-width:100%;overflow-x:hidden;-webkit-font-smoothing:antialiased;text-rendering:optimizeLegibility}
::selection{background:var(--accent-soft);color:var(--text-emphasis)}
:focus{outline:none}
:focus-visible{outline:2px solid var(--brand-500);outline-offset:2px;border-radius:6px}

.skip-link{position:absolute;left:-9999px;top:auto;width:1px;height:1px;overflow:hidden}
.skip-link:focus{position:fixed;left:16px;top:16px;width:auto;height:auto;z-index:100;padding:10px 16px;background:var(--surface);color:var(--brand-700);border:2px solid var(--brand-500);border-radius:8px;font-weight:600;text-decoration:none;box-shadow:var(--shadow-lg)}

.page{width:100%;max-width:min(1640px,100%);margin:0 auto;padding:32px 28px 24px;min-width:0;position:relative}
.page::before{content:"";position:fixed;inset:0;pointer-events:none;background:var(--halo);z-index:0}
.page>*{position:relative;z-index:1}
)CSS"
    R"CSS(
.report-header{position:relative;padding:32px 32px 28px;margin:0 0 22px;border:1px solid var(--border);border-radius:var(--radius-lg);background:linear-gradient(180deg,var(--surface),var(--surface-elev));overflow:hidden;box-shadow:var(--shadow-md)}
.report-header::before{content:"";position:absolute;left:0;right:0;top:0;height:3px;background:var(--brand-gradient)}
.report-header::after{content:"";position:absolute;right:-120px;top:-120px;width:340px;height:340px;border-radius:50%;background:var(--brand-gradient);filter:blur(60px);opacity:.10;pointer-events:none}
.report-brand{display:flex;align-items:center;gap:12px;margin:0 0 14px}
)CSS"
    R"CSS(
.report-brand .wordmark{font-size:17px;font-weight:800;letter-spacing:.01em;color:var(--text-emphasis)}
.report-brand .wordmark-sub{color:var(--text-subtle);font-size:12px;font-weight:500;font-family:ui-monospace,SFMono-Regular,'JetBrains Mono','SF Mono',Menlo,Consolas,monospace;letter-spacing:.06em;padding:2px 8px;background:var(--surface-muted);border-radius:999px;border:1px solid var(--border)}
h1{margin:0 0 10px;font-size:38px;font-weight:800;letter-spacing:-.02em;line-height:1.1;color:var(--text-emphasis)}
h1 .grad{background:var(--brand-gradient);-webkit-background-clip:text;background-clip:text;color:transparent}
.headline-sub{margin:0 0 18px;color:var(--text-muted);font-size:15px;line-height:1.55;max-width:78ch}
.report-meta{margin:0;color:var(--text-muted);font-size:13.5px;display:flex;flex-wrap:wrap;gap:6px 16px;align-items:center}
.report-meta code{font-size:12.5px}
.meta-sep{color:var(--text-subtle);opacity:.6}
.toolbar-cluster{position:absolute;top:18px;right:18px;display:inline-flex;flex-wrap:wrap;gap:8px;z-index:3}
.summary-strip{display:flex;flex-wrap:wrap;gap:8px;margin:18px 0 0;padding-top:14px;border-top:1px dashed var(--border)}
.summary-pill{display:inline-flex;align-items:center;gap:8px;padding:7px 14px;border-radius:999px;border:1px solid var(--border);background:var(--surface-muted);font-size:13px;font-weight:600;color:var(--text-muted);font-variant-numeric:tabular-nums;line-height:1}
.summary-pill::before{content:"";display:inline-block;width:9px;height:9px;border-radius:50%;background:currentColor;flex:none}
.summary-pill .pill-num{color:var(--text-emphasis);font-size:14.5px;font-weight:800}
.summary-pill.is-pass{background:var(--ok-bg);color:var(--ok-fg);border-color:transparent}
.summary-pill.is-pass .pill-num{color:var(--ok-fg)}
.summary-pill.is-warn{background:var(--warn-bg);color:var(--warn-fg);border-color:transparent}
.summary-pill.is-warn .pill-num{color:var(--warn-fg)}
.summary-pill.is-fail{background:var(--fail-bg);color:var(--fail-fg);border-color:transparent}
.summary-pill.is-fail .pill-num{color:var(--fail-fg)}
.summary-pill.is-neutral{background:var(--surface-muted)}
.summary-pill.is-neutral::before{background:var(--text-subtle)}
)CSS"
    R"CSS(
.section-nav{position:sticky;top:10px;z-index:10;display:flex;flex-wrap:wrap;align-items:center;gap:6px;background:color-mix(in srgb,var(--surface) 80%,transparent);backdrop-filter:saturate(180%) blur(14px);-webkit-backdrop-filter:saturate(180%) blur(14px);border:1px solid var(--border);border-radius:999px;box-shadow:var(--shadow-md);margin:0 0 26px;padding:6px;max-width:100%;overflow-x:auto;overflow-y:hidden;-webkit-overflow-scrolling:touch;scrollbar-width:thin}
.section-nav a{position:relative;display:inline-flex;align-items:center;gap:8px;justify-content:center;padding:10px 18px;font-size:15px;font-weight:600;line-height:1.4;color:var(--text-muted);text-decoration:none;white-space:nowrap;border:1px solid transparent;border-radius:999px;transition:background .18s ease,color .18s ease,border-color .18s ease,transform .12s ease}
.section-nav a:hover{background:var(--accent-soft);color:var(--brand-600)}
.section-nav a:active{transform:scale(.96)}
.section-nav a.is-active,.section-nav a[aria-current="true"]{background:var(--brand-gradient);color:#fff;box-shadow:0 4px 14px rgba(99,102,241,.32)}
.section-nav a.is-active:hover{color:#fff}

.hero{display:grid;grid-template-columns:repeat(auto-fit,minmax(160px,1fr));gap:12px;margin-bottom:24px}
.card{position:relative;background:var(--surface);border:1px solid var(--border);border-radius:var(--radius-md);padding:18px 18px 16px;box-shadow:var(--shadow-sm);transition:transform .15s ease,box-shadow .15s ease,border-color .15s ease}
.card:hover{border-color:var(--border-strong);box-shadow:var(--shadow-md)}
.card::before{content:"";position:absolute;left:0;top:0;bottom:0;width:3px;background:var(--brand-gradient);border-radius:var(--radius-md) 0 0 var(--radius-md);opacity:.16}
.card.is-pass::before{background:linear-gradient(180deg,var(--ok),var(--accent-3));opacity:.32}
.card.is-warn::before{background:linear-gradient(180deg,var(--warn),#f59e0b);opacity:.32}
.card.is-fail::before{background:linear-gradient(180deg,var(--fail),#ef4444);opacity:.32}
.card .label{font-size:12px;font-weight:700;color:var(--text-subtle);text-transform:uppercase;letter-spacing:.10em}
.card .value{font-size:32px;font-weight:800;margin-top:6px;color:var(--text-emphasis);font-variant-numeric:tabular-nums;letter-spacing:-.02em;line-height:1}

.panel{position:relative;background:var(--surface);border:1px solid var(--border);border-radius:var(--radius-md);padding:26px 28px;margin-bottom:20px;box-shadow:var(--shadow-sm);overflow:visible;min-width:0;scroll-margin-top:84px}
h2{margin:0 0 16px;font-size:23px;font-weight:800;letter-spacing:-.015em;color:var(--text-emphasis);display:flex;align-items:center;gap:14px;line-height:1.2}
h2 .h-anchor{flex:none;display:inline-flex;align-items:center;justify-content:center;width:42px;height:42px;border-radius:12px;background:var(--brand-gradient);color:#fff;font-family:ui-monospace,SFMono-Regular,'JetBrains Mono','SF Mono',Menlo,Consolas,monospace;font-size:15px;font-weight:800;letter-spacing:.02em;box-shadow:0 6px 16px rgba(67,56,202,.28),inset 0 0 0 1px rgba(255,255,255,.22);position:relative;overflow:hidden}
h2 .h-anchor::after{content:"";position:absolute;inset:1px;border-radius:11px;background:linear-gradient(180deg,rgba(255,255,255,.18),transparent 55%);pointer-events:none}
.panel-head{display:flex;align-items:center;justify-content:space-between;gap:14px;flex-wrap:wrap;margin-bottom:14px}
.panel-head h2{margin:0}
.note{margin:0 0 16px;color:var(--text-muted);font-size:14.5px;line-height:1.7}
.note code{font-size:13px}
.note strong{color:var(--text-emphasis)}
)CSS"
    R"CSS(
.toggle-button{appearance:none;border:1px solid var(--border-strong);cursor:pointer;padding:7px 14px;border-radius:8px;background:var(--surface);color:var(--text);font-size:12.5px;font-weight:600;font-family:inherit;transition:background .15s,border-color .15s,color .15s}
.toggle-button:hover{background:var(--accent-soft);border-color:var(--brand-500);color:var(--brand-700)}
.toggle-panel{display:none;margin-top:18px;padding-top:18px;border-top:1px dashed var(--border)}

.bench-table{width:100%;border-collapse:separate;border-spacing:0;font-size:13.5px;font-variant-numeric:tabular-nums}
.bench-table thead th{position:sticky;top:0;background:var(--surface-muted);border-bottom:1px solid var(--border-strong);padding:12px 13px;text-align:left;font-weight:700;color:var(--text-subtle);font-size:11.5px;letter-spacing:.08em;text-transform:uppercase;white-space:nowrap}
.bench-table thead th:first-child{border-top-left-radius:8px}
.bench-table thead th:last-child{border-top-right-radius:8px}
.bench-table tbody td{border-bottom:1px solid var(--border);padding:12px 13px;vertical-align:top;color:var(--text);white-space:nowrap;transition:background .12s ease}
.bench-table tbody td.config-cell,.bench-table tbody td[data-label="Errors"]{white-space:normal;overflow-wrap:anywhere;word-break:break-word;min-width:280px}
.bench-table tbody tr:nth-child(even) td{background:var(--row-zebra)}
.bench-table tbody tr:hover td{background:var(--accent-soft)}
.bench-table tbody tr:last-child td{border-bottom:none}
.config-cell{min-width:320px;line-height:1.5}
.config-cell div{margin:2px 0}
.config-cell strong{font-size:10.5px;text-transform:uppercase;letter-spacing:.06em;color:var(--text-subtle);margin-right:6px}

.bench-table td.ok,.ok{color:var(--ok-fg);font-weight:600}
.bench-table td.warn,.warn{color:var(--warn-fg);font-weight:600}
.bench-table td.fail,.fail{color:var(--fail-fg);font-weight:600}
.bench-table td.muted,.muted{color:var(--text-muted);font-weight:500}
.bench-table td.ok{border-left:3px solid var(--ok)}
.bench-table td.warn{border-left:3px solid var(--warn)}
.bench-table td.fail{border-left:3px solid var(--fail)}

code{font-family:ui-monospace,SFMono-Regular,'JetBrains Mono','SF Mono',Menlo,Consolas,monospace;background:var(--code-bg);color:var(--text);padding:1.5px 7px;border-radius:5px;font-size:.9em;border:1px solid var(--border)}
pre code{background:none;border:none;padding:0}

.insight-item{position:relative;padding:20px 22px 20px 70px;margin:16px 0;border:1px solid var(--border);border-radius:var(--radius-md);background:linear-gradient(180deg,var(--surface) 0%,color-mix(in srgb,var(--surface-muted) 100%,var(--surface)) 100%);color:var(--text);box-shadow:var(--shadow-sm);transition:transform .15s ease,border-color .15s ease,box-shadow .15s ease}
.insight-item::before{content:"";position:absolute;left:0;top:0;bottom:0;width:5px;border-radius:var(--radius-md) 0 0 var(--radius-md);background:var(--brand-gradient)}
.insight-item::after{content:"\2605";position:absolute;left:18px;top:50%;transform:translateY(-50%);width:38px;height:38px;display:flex;align-items:center;justify-content:center;background:var(--brand-gradient);color:#fff;font-size:17px;font-weight:800;border-radius:11px;box-shadow:0 6px 18px rgba(67,56,202,.32),inset 0 0 0 1px rgba(255,255,255,.22)}
.insight-item:nth-of-type(2)::after{content:"\26A1"}
.insight-item:nth-of-type(3)::after{content:"\1F4E1"}
.insight-item:nth-of-type(4)::after{content:"\1F500"}
.insight-item:nth-of-type(5)::after{content:"\1F4E6"}
.insight-item:hover{transform:translateY(-2px);border-color:var(--brand-300);box-shadow:var(--shadow-md)}
.insight-item div{margin:5px 0;line-height:1.65;font-size:14.5px}
.insight-item div:first-child{font-size:15.5px;font-weight:600;color:var(--text-emphasis)}
.insight-item strong{color:var(--brand-700);font-weight:700;font-size:11px;text-transform:uppercase;letter-spacing:.08em;margin-right:8px}
.insight-item code{font-size:13px;background:var(--code-bg)}

.seg-toggle{display:inline-flex;gap:2px;padding:3px;background:var(--surface-muted);border:1px solid var(--border);border-radius:999px;box-shadow:var(--shadow-sm)}
.seg-btn{appearance:none;border:0;background:transparent;padding:6px 12px;font:inherit;font-size:12px;font-weight:700;color:var(--text-muted);cursor:pointer;border-radius:999px;transition:all .15s ease;display:inline-flex;align-items:center;gap:5px;min-width:38px;justify-content:center}
.seg-btn:hover{color:var(--text-emphasis);background:color-mix(in srgb,var(--surface) 60%,transparent)}
.seg-btn.is-on{background:var(--surface);color:var(--brand-700);box-shadow:0 1px 3px rgba(15,23,42,.08),inset 0 0 0 1px var(--border-strong)}
.seg-btn svg{display:block}
)CSS"
    R"CSS(
.panel svg{display:block;max-width:100%;height:auto;border-radius:8px;border:1px solid var(--border);background:#ffffff}
.chart-card{margin:6px 0 0}
.chart-toolbar{display:flex;align-items:center;gap:6px;margin:0 0 10px;flex-wrap:wrap}
.chart-btn{appearance:none;border:1px solid var(--border-strong);background:var(--surface);color:var(--text);font:700 13px/1 inherit;padding:7px 11px;border-radius:8px;cursor:pointer;min-width:32px;transition:background .15s,border-color .15s,color .15s,transform .12s;font-variant-numeric:tabular-nums}
.chart-btn:hover{background:var(--accent-soft);border-color:var(--brand-400);color:var(--brand-700)}
.chart-btn:active{transform:scale(.96)}
.chart-hint{margin-left:auto;font-size:11.5px;color:var(--text-subtle);font-family:ui-monospace,SFMono-Regular,'JetBrains Mono','SF Mono',Menlo,Consolas,monospace;letter-spacing:.02em}
.bench-chart{touch-action:none;user-select:none;cursor:crosshair;text-rendering:geometricPrecision}
.bench-chart text{paint-order:stroke fill}
.bench-chart.is-panning{cursor:grabbing}
.bench-chart .chart-line{transition:opacity .15s;stroke-linecap:round;stroke-linejoin:round;vector-effect:non-scaling-stroke}
.bench-chart .chart-line.dim{opacity:.12}
.bench-chart .chart-line.hidden,.bench-chart .chart-point.hidden{display:none}
.bench-chart .chart-point{vector-effect:non-scaling-stroke}
.bench-chart .chart-crosshair-line{vector-effect:non-scaling-stroke}
.bench-chart .chart-point-halo{vector-effect:non-scaling-stroke}
.chart-tooltip{position:fixed;z-index:60;pointer-events:none;max-width:300px;background:var(--surface);color:var(--text);border:1px solid var(--border-strong);border-radius:10px;box-shadow:var(--shadow-lg);padding:10px 12px;font-size:12px;line-height:1.5;opacity:0;transform:translate(-50%,calc(-100% - 10px));transition:opacity .12s ease;font-variant-numeric:tabular-nums}
.chart-tooltip.show{opacity:1}
.chart-tooltip .tt-head{font-weight:700;color:var(--brand-600);margin-bottom:6px;padding-bottom:4px;border-bottom:1px solid var(--border);font-family:ui-monospace,SFMono-Regular,'JetBrains Mono','SF Mono',Menlo,Consolas,monospace;font-size:11.5px}
.chart-tooltip .tt-row{display:flex;align-items:center;gap:8px;margin:3px 0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.chart-tooltip .tt-dot{width:10px;height:10px;border-radius:3px;flex:none}
.chart-tooltip .tt-label{color:var(--text-muted);flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis}
.chart-tooltip .tt-value{color:var(--text-emphasis);font-weight:700}
.legend-map{display:grid;grid-template-columns:repeat(auto-fit,minmax(280px,1fr));gap:8px;margin-top:16px;font-size:12.5px;color:var(--text)}
.legend-entry{display:flex;gap:10px;align-items:flex-start;padding:9px 12px;width:100%;background:var(--surface-muted);border:1px solid var(--border);border-radius:8px;cursor:pointer;text-align:left;color:inherit;font:inherit;transition:background .15s,border-color .15s,opacity .15s,transform .12s}
.legend-entry:hover{border-color:var(--brand-400);background:var(--accent-soft);transform:translateX(2px)}
.legend-entry[aria-pressed="false"]{opacity:.42}
.legend-entry[aria-pressed="false"] .legend-color line{opacity:.35}
.legend-color{display:inline-block;width:26px;height:12px;margin-top:4px;flex:none}
.legend-id{min-width:36px;flex:none;color:var(--text-subtle);font-weight:700;font-size:11.5px}
.legend-id code{background:transparent;border:0;padding:0}
.legend-entry span{word-break:break-word}

.panel ul{margin:0;padding-left:22px;color:var(--text-muted);line-height:1.85}
.panel ul li{padding:3px 0}

hr{border:none;border-top:1px dashed var(--border);margin:26px 0}

.table-scroll{overflow-x:auto;-webkit-overflow-scrolling:touch;margin:0 -2px;max-width:100%;min-width:0;scrollbar-width:thin;scroll-snap-type:x proximity;border-radius:10px}
.table-scroll .bench-table thead th{scroll-snap-align:start}
.chart-scroll{overflow-x:auto;-webkit-overflow-scrolling:touch;margin:0 -2px;scrollbar-width:thin;border-radius:8px}
.chart-scroll svg{min-width:640px}

.config-cell,.config-cell code{overflow-wrap:anywhere;word-break:break-word;white-space:normal}
.bench-table tbody td code{white-space:nowrap}
.bench-table tbody td.config-cell code,.bench-table tbody td[data-label="Errors"] code{white-space:normal;overflow-wrap:anywhere}
)CSS"
    R"CSS(
.decision-panel{position:relative;border:1px solid var(--border);padding:26px 28px;background:linear-gradient(135deg,color-mix(in srgb,var(--brand-500) 8%,transparent),color-mix(in srgb,var(--accent-2) 5%,transparent));margin-bottom:24px;overflow:hidden;box-shadow:var(--shadow-md)}
.decision-panel::before{content:"";position:absolute;inset:0;border-radius:inherit;padding:1px;background:var(--brand-gradient);-webkit-mask:linear-gradient(#000 0 0) content-box,linear-gradient(#000 0 0);-webkit-mask-composite:xor;mask:linear-gradient(#000 0 0) content-box,linear-gradient(#000 0 0);mask-composite:exclude;pointer-events:none;opacity:.5}
.decision-panel h2{margin:0 0 10px;font-size:23px;color:var(--text-emphasis)}
.decision-panel .host-fingerprint{margin:0 0 14px;color:var(--text-muted);font-size:13px}
.recommendation-lead{margin:0 0 18px;padding:16px 18px 16px 56px;border:1px solid color-mix(in srgb,var(--brand-500) 30%,transparent);border-radius:var(--radius-md);background:color-mix(in srgb,var(--brand-500) 10%,var(--surface));color:var(--text);font-size:14.5px;line-height:1.7;position:relative;box-shadow:var(--shadow-sm)}
.recommendation-lead::before{content:"\2605";position:absolute;left:14px;top:50%;transform:translateY(-50%);width:30px;height:30px;display:flex;align-items:center;justify-content:center;background:var(--brand-gradient);color:#fff;border-radius:50%;font-size:14px;font-weight:700;box-shadow:0 4px 12px rgba(67,56,202,.32),inset 0 0 0 1px rgba(255,255,255,.22)}
.recommendation-lead strong{color:var(--brand-700);font-weight:800}
.decision-panel .layer-group{margin:14px 0 6px;padding:14px 16px;background:var(--surface);border-radius:var(--radius-md);border:1px solid var(--border);box-shadow:var(--shadow-sm)}
.decision-panel .layer-group h3{margin:0 0 10px;font-size:13px;color:var(--brand-700);font-weight:700;text-transform:uppercase;letter-spacing:.06em}
.decision-panel .transport-card{display:flex;flex-wrap:wrap;align-items:center;gap:10px;padding:11px 14px;margin:6px 0;background:var(--surface);border:1px solid var(--border);border-radius:var(--radius-md);transition:border-color .15s,box-shadow .15s,transform .15s}
.decision-panel .transport-card:hover{border-color:var(--brand-400);box-shadow:var(--shadow-md)}
.decision-panel .transport-card:first-of-type{border-color:var(--brand-500);box-shadow:0 4px 14px rgba(99,102,241,.18)}
.decision-panel .medal{font-size:15px;min-width:48px;font-weight:700;color:var(--brand-700);font-family:ui-monospace,SFMono-Regular,'JetBrains Mono','SF Mono',Menlo,Consolas,monospace}
.decision-panel .medal-glyph{font-size:20px;line-height:1;margin-right:-2px;filter:drop-shadow(0 1px 1px rgba(0,0,0,.18))}
.decision-panel .transport-name{font-size:13.5px;font-weight:700;color:var(--text-emphasis);min-width:118px;padding:4px 9px;background:var(--surface-muted);border-radius:6px;border:1px solid var(--border)}
.decision-panel .transport-url{font-size:11px;color:var(--text-muted);max-width:360px;overflow-wrap:anywhere}

.score-pill{display:inline-flex;align-items:center;gap:5px;padding:4px 11px;border-radius:999px;font-size:12px;font-weight:700;background:var(--accent-soft);color:var(--brand-700);transition:transform .12s}
.score-pill:hover{transform:translateY(-1px)}
.decision-panel .score-pill.speed{background:color-mix(in srgb,var(--brand-500) 14%,transparent);color:var(--brand-700)}
.decision-panel .score-pill.capacity{background:color-mix(in srgb,var(--accent-3) 18%,transparent);color:var(--ok-fg)}
.decision-panel .score-pill.efficiency{background:color-mix(in srgb,var(--warn) 22%,transparent);color:var(--warn-fg)}
.decision-panel .score-pill.coverage{background:color-mix(in srgb,var(--accent-2) 18%,transparent);color:var(--brand-700)}
.score-pill.loss{background:var(--warn-bg);color:var(--warn-fg)}
.score-pill.loss.loss-high{background:var(--fail-bg);color:var(--fail-fg)}
.decision-panel .score-sum{padding:6px 16px;border-radius:999px;background:linear-gradient(135deg,hsl(var(--score-hue,80) calc(58% + var(--score-pct,80) * 0.07%) calc(48% - var(--score-pct,80) * 0.10%)),hsl(var(--score-hue,80) calc(50% + var(--score-pct,80) * 0.10%) calc(58% - var(--score-pct,80) * 0.10%)));color:#fff;font-size:16px;font-weight:800;letter-spacing:-.01em;box-shadow:0 4px 14px hsl(var(--score-hue,80) 65% 40% / .35),inset 0 0 0 1px rgba(255,255,255,.18);display:inline-flex;align-items:baseline;gap:3px;min-width:80px;justify-content:center}
.decision-panel .score-sum .score-sum-max{font-size:11px;font-weight:600;opacity:.78;letter-spacing:.02em}
.decision-panel .advantage{padding:4px 11px;border-radius:999px;background:color-mix(in srgb,var(--accent-3) 22%,transparent);color:var(--ok-fg);font-size:12px;font-weight:700}
.decision-panel .confidence{padding:3px 10px;border-radius:999px;font-size:11px;font-weight:700;margin-left:auto;letter-spacing:.02em}
.confidence{padding:3px 10px;border-radius:999px;font-size:11px;font-weight:700}
.confidence-high{background:var(--ok-bg);color:var(--ok-fg)}
.confidence-medium{background:var(--warn-bg);color:var(--warn-fg)}
.confidence-noisy{background:var(--fail-bg);color:var(--fail-fg)}
.confidence-solo{background:color-mix(in srgb,var(--text-subtle) 18%,transparent);color:var(--text-subtle)}
.confidence-unknown{background:color-mix(in srgb,var(--text-subtle) 18%,transparent);color:var(--text-subtle)}

.toolbar-cluster .seg-toggle{box-shadow:var(--shadow-md)}
@media (max-width:640px){.toolbar-cluster{position:static;margin:0 0 12px;flex-wrap:wrap;justify-content:flex-end}}
.decision-panel .degrade-note{margin:6px 0 0;font-size:12px;color:var(--text-subtle);font-style:italic}
)CSS"
    R"CSS(
#health summary{cursor:pointer;list-style:none;padding:4px 0;outline:none;display:flex;align-items:center;flex-wrap:wrap;gap:6px}
#health summary::-webkit-details-marker{display:none}
#health summary::before{content:"";display:inline-block;width:8px;height:8px;border-right:2px solid var(--text-subtle);border-bottom:2px solid var(--text-subtle);transform:rotate(-45deg);margin:0 8px 0 2px;transition:transform .18s ease}
#health details[open] summary::before{transform:rotate(45deg)}
#health .health-badge{display:inline-flex;align-items:center;gap:5px;padding:3px 11px;border-radius:999px;font-size:11px;font-weight:700;margin-left:8px;vertical-align:middle;letter-spacing:.04em;text-transform:uppercase}
#health .health-badge::before{content:"";width:7px;height:7px;border-radius:50%;background:currentColor}
#health .health-ok{background:var(--ok-bg);color:var(--ok-fg)}
#health .health-warn{background:var(--warn-bg);color:var(--warn-fg)}
#health .health-attention{background:var(--fail-bg);color:var(--fail-fg)}
#health h3{font-size:12px;color:var(--brand-700);font-weight:700;letter-spacing:.10em;text-transform:uppercase;margin:18px 0 8px}

.heatmap-panel{margin-bottom:24px}
.heatmap-block+.heatmap-block{margin-top:24px;padding-top:20px;border-top:1px dashed var(--border)}
.heatmap-scroll{max-width:100%;overflow-x:auto;-webkit-overflow-scrolling:touch;scrollbar-width:thin;border-radius:10px;border:1px solid var(--border)}
.heatmap-grid{display:grid;background:var(--surface);width:max-content;min-width:100%}
.heatmap-row{display:grid;border-bottom:1px solid var(--border)}
.heatmap-row:last-child{border-bottom:0}
.heatmap-row.heatmap-head{background:var(--surface-muted);font-weight:700;font-size:10.5px;color:var(--text-subtle);text-transform:uppercase;letter-spacing:.08em}
.heatmap-cell{padding:9px 11px;border-right:1px solid var(--border);position:relative;font-size:12px;display:flex;align-items:center;overflow:hidden;transition:background .15s}
.heatmap-cell:last-child{border-right:0}
.heatmap-cell.head-transport{text-align:left}
.heatmap-cell.head-size{justify-content:center;font-weight:700}
.heatmap-cell.name code{font-weight:700;color:var(--text-emphasis);background:var(--code-bg);border:1px solid var(--border);padding:2px 7px;border-radius:5px}
.heatmap-cell.name{align-items:flex-start;flex-direction:column;gap:4px}
.heatmap-cell.name small{color:var(--text-subtle);font-size:10px;line-height:1.2;overflow-wrap:anywhere}
.heatmap-cell.empty{color:var(--text-subtle);justify-content:center;background:repeating-linear-gradient(45deg,transparent,transparent 8px,var(--row-zebra) 8px,var(--row-zebra) 16px)}
.heatmap-cell.empty.heatmap-fail{color:var(--fail-fg);background:var(--fail-bg);font-weight:700}
.heatmap-cell.score{justify-content:flex-end;padding-right:10px;min-height:46px}
.heatmap-cell.score.latency{display:grid;grid-template-columns:auto 1fr;align-items:stretch;text-align:center;padding:8px 10px;min-height:80px;gap:10px;background:linear-gradient(180deg,color-mix(in srgb,var(--surface-muted) 40%,transparent),color-mix(in srgb,var(--surface-muted) 80%,transparent));border-bottom:2px solid var(--border)}
.heatmap-cell.score.throughput{justify-content:flex-end}
.heatmap-cell.score.latency .lat-meters{display:flex;flex-direction:row;align-items:stretch;gap:4px;align-self:stretch}
.heatmap-latency-meter{position:relative;width:10px;align-self:stretch;min-height:60px;border-radius:999px;background:color-mix(in srgb,var(--text-subtle) 14%,transparent);overflow:hidden;border:1px solid var(--border)}
.heatmap-latency-fill{--heat-pct:50;--heat-hue:calc(120deg - var(--heat-pct) * 1.2deg);position:absolute;left:0;right:0;bottom:0;border-radius:999px 999px 999px 999px;min-height:4px;opacity:.88;transition:opacity .15s;background:linear-gradient(180deg,hsl(var(--heat-hue) 62% 46%),hsl(var(--heat-hue) 72% 28%))}
.heatmap-bar{position:absolute;top:0;left:0;height:100%;opacity:.85;transition:opacity .15s;mix-blend-mode:multiply}
.heatmap-bar.heat-strong{background:linear-gradient(90deg,color-mix(in srgb,var(--heat-strong) 55%,transparent),color-mix(in srgb,var(--heat-strong) 92%,#000))}
.heatmap-bar.heat-med{background:linear-gradient(90deg,color-mix(in srgb,var(--heat-mid) 55%,transparent),color-mix(in srgb,var(--heat-mid) 92%,#000))}
.heatmap-bar.heat-weak{background:linear-gradient(90deg,color-mix(in srgb,var(--heat-weak) 55%,transparent),color-mix(in srgb,var(--heat-weak) 92%,#000))}
.heatmap-cell.score.throughput .heatmap-num strong{color:#fff;text-shadow:0 1px 2px rgba(0,0,0,.55),0 0 4px rgba(0,0,0,.35);font-weight:800}
.heatmap-cell.score.throughput .heatmap-num small{color:#fff;text-shadow:0 1px 2px rgba(0,0,0,.55);opacity:.92}
.heatmap-legend{display:flex;flex-wrap:wrap;gap:8px;margin:0 0 14px}
.heat-chip{display:inline-flex;align-items:center;gap:5px;min-height:24px;padding:3px 11px;border-radius:999px;font-size:11px;font-weight:700;border:1px solid var(--border)}
.heat-chip::before{content:"";width:8px;height:8px;border-radius:50%;display:inline-block}
.heat-chip.heat-strong{background:color-mix(in srgb,var(--heat-strong) 22%,transparent);color:var(--ok-fg)}
.heat-chip.heat-strong::before{background:var(--heat-strong)}
.heat-chip.heat-med{background:color-mix(in srgb,var(--heat-mid) 22%,transparent);color:var(--warn-fg)}
.heat-chip.heat-med::before{background:var(--heat-mid)}
.heat-chip.heat-weak{background:color-mix(in srgb,var(--heat-weak) 22%,transparent);color:var(--fail-fg)}
.heat-chip.heat-weak::before{background:var(--heat-weak)}
.heatmap-cell:hover{background:var(--accent-soft)}
.heatmap-cell:hover .heatmap-bar{opacity:calc(var(--heat-opacity) + .20)}
.heatmap-cell.score.latency:hover .heatmap-latency-fill{opacity:1}
.heatmap-num{position:relative;z-index:1;display:flex;flex-direction:column;align-items:flex-end;justify-content:center;font-weight:800;color:var(--text-emphasis);line-height:1.15}
.heatmap-cell.score.latency .heatmap-num{align-items:stretch;justify-content:center;text-align:left;gap:1px}
.heatmap-cell.score.latency .heatmap-num.lat-stack{display:flex;flex-direction:column}
.heatmap-cell.score.latency .lat-row{display:grid;grid-template-columns:38px 1fr auto;align-items:baseline;column-gap:4px;font-size:11.5px;line-height:1.3}
.heatmap-cell.score.latency .lat-row .lat-tag{font-size:9.5px;font-weight:700;color:var(--text-subtle);letter-spacing:.04em;text-transform:uppercase;font-family:ui-monospace,SFMono-Regular,'JetBrains Mono','SF Mono',Menlo,Consolas,monospace}
.heatmap-cell.score.latency .lat-row strong{font-size:12px;font-weight:700;color:var(--text-emphasis);font-variant-numeric:tabular-nums;text-align:right}
.heatmap-cell.score.latency .lat-row .lat-unit{font-size:9.5px;color:var(--text-subtle);font-family:ui-monospace,SFMono-Regular,'JetBrains Mono','SF Mono',Menlo,Consolas,monospace}
.heatmap-cell.score.latency .heatmap-num small{margin-top:3px;font-size:10px;color:var(--text-subtle);text-align:right}
.heatmap-num small{font-size:10px;font-weight:600;color:var(--text-muted);margin-top:3px;letter-spacing:.02em}

.chart-panel summary{cursor:pointer;list-style:none;padding:6px 0;outline:none;user-select:none}
.chart-panel summary::-webkit-details-marker{display:none}
.chart-panel summary::before{content:"";display:inline-block;width:8px;height:8px;border-right:2px solid var(--text-subtle);border-bottom:2px solid var(--text-subtle);transform:rotate(-45deg);margin:0 10px 0 2px;transition:transform .18s ease;vertical-align:middle}
.chart-panel details[open] summary::before{transform:rotate(45deg)}
.chart-panel details[open] summary h2{color:var(--brand-700)}
.profile-formula{margin:12px 0;border:1px solid var(--border);border-radius:var(--radius-sm);background:var(--surface-muted);padding:0}
.profile-formula summary{cursor:pointer;list-style:none;padding:10px 14px;outline:none;user-select:none;font-weight:600;color:var(--text-emphasis);display:flex;align-items:center}
.profile-formula summary::-webkit-details-marker{display:none}
.profile-formula summary::before{content:"";display:inline-block;width:8px;height:8px;border-right:2px solid var(--brand-500);border-bottom:2px solid var(--brand-500);transform:rotate(-45deg);margin-right:10px;transition:transform .18s ease;vertical-align:middle}
.profile-formula details[open] summary::before,.profile-formula[open] summary::before{transform:rotate(45deg)}
.profile-formula .formula-list{margin:0;padding:6px 16px 12px 32px;list-style:disc}
.profile-formula .formula-list li{margin:6px 0;line-height:1.65;color:var(--text)}
.profile-formula .formula-equation{margin:8px 16px 14px;padding:10px 12px;border-top:1px dashed var(--border);font-family:var(--font-mono);font-size:13px;color:var(--text-emphasis)}
.profile-formula .formula-note{margin:0 16px 14px;padding:0;font-size:13px;color:var(--text-muted);line-height:1.6}

.profile-panel{margin-bottom:24px}
.profile-card{position:relative;margin:10px 0;padding:0;border:1px solid var(--border);border-left:5px solid hsl(var(--score-hue,80) calc(55% + var(--score-pct,80) * 0.08%) calc(50% - var(--score-pct,80) * 0.12%));border-radius:var(--radius-md);background:linear-gradient(90deg,hsl(var(--score-hue,80) calc(55% + var(--score-pct,80) * 0.10%) calc(55% - var(--score-pct,80) * 0.05%) / .12) 0%,hsl(var(--score-hue,80) 60% 50% / .04) 26%,var(--surface) 60%);transition:box-shadow .15s,transform .15s}
.profile-card::before{content:"";position:absolute;left:0;top:0;bottom:0;width:5px;background:linear-gradient(180deg,hsl(var(--score-hue,80) calc(58% + var(--score-pct,80) * 0.07%) calc(48% - var(--score-pct,80) * 0.10%)),hsl(var(--score-hue,80) calc(50% + var(--score-pct,80) * 0.10%) calc(58% - var(--score-pct,80) * 0.10%)));border-radius:var(--radius-md) 0 0 var(--radius-md);pointer-events:none}
.profile-card:hover{box-shadow:var(--shadow-md);transform:translateY(-1px)}
.decision-panel .profile-card:first-of-type{box-shadow:0 6px 22px hsl(var(--score-hue,80) 60% 40% / .22)}
.profile-card[open]{box-shadow:var(--shadow-md)}
.profile-card summary{cursor:pointer;list-style:none;padding:14px 16px;outline:none}
.profile-card summary::-webkit-details-marker{display:none}
.profile-card summary::before{content:"";display:inline-block;width:8px;height:8px;border-right:2px solid var(--text-subtle);border-bottom:2px solid var(--text-subtle);transform:rotate(-45deg);margin:0 12px 0 2px;transition:transform .18s ease;vertical-align:middle}
.profile-card[open] summary::before{transform:rotate(45deg)}
.profile-head{display:flex;align-items:center;flex-wrap:wrap;gap:9px}
.profile-name{font-size:13.5px;font-weight:800;color:var(--text-emphasis);padding:4px 10px;background:var(--surface-muted);border-radius:6px;min-width:108px;border:1px solid var(--border)}
.profile-layer{font-size:11px;padding:3px 9px;border-radius:999px;background:color-mix(in srgb,var(--brand-500) 12%,transparent);color:var(--brand-700);font-weight:700;letter-spacing:.02em}
.profile-body{padding:4px 16px 16px 38px;display:grid;grid-template-columns:1fr 1fr;gap:16px}
@media (max-width:640px){.profile-body{grid-template-columns:1fr;padding-left:18px}}
.profile-facts,.profile-numbers{background:var(--surface-muted);border-radius:var(--radius-md);padding:12px 16px;border:1px solid var(--border)}
.merged-h3{margin:24px 0 12px;padding:10px 14px;border-left:4px solid var(--brand-500);border-radius:0 var(--radius-sm) var(--radius-sm) 0;background:linear-gradient(90deg,var(--accent-soft),transparent 70%);font-size:17px;font-weight:700;letter-spacing:-.005em;color:var(--text-emphasis);scroll-margin-top:84px}
)CSS"
    R"CSS(
.fact-row,.num-row{display:flex;align-items:baseline;gap:12px;padding:5px 0;font-size:13px;border-bottom:1px dashed color-mix(in srgb,var(--border) 60%,transparent)}
.fact-row:last-child,.num-row:last-child{border-bottom:0}
.fact-label,.num-label{color:var(--text-subtle);font-weight:700;min-width:108px;font-size:10.5px;text-transform:uppercase;letter-spacing:.08em}
.fact-val,.num-val{color:var(--text-emphasis);font-weight:600}
.num-val{font-variant-numeric:tabular-nums;font-weight:800;color:var(--brand-700);font-size:14px}
.num-ctx{color:var(--text-subtle);font-size:11px;margin-left:auto;font-style:italic}

.bench-footer{margin:48px 0 0;padding:24px 26px;border:1px dashed var(--border-strong);border-top:2px solid var(--border-strong);border-radius:var(--radius-md);background:var(--surface);font-size:12.5px;color:var(--text-subtle);display:flex;flex-wrap:wrap;align-items:center;gap:8px 22px;justify-content:space-between}
.bench-footer strong{color:var(--text-muted);font-weight:700}
.bench-footer code{font-size:11.5px}

@media (prefers-reduced-motion:reduce){html{scroll-behavior:auto}.section-nav a,.toggle-button,.card,.profile-card,.heatmap-cell,.chart-btn,.seg-btn,.legend-entry{transition:none}.recommendation-lead::before,.report-header::after{animation:none}}

@media (max-width:980px){
  .page{padding:24px 22px 24px}
  h1{font-size:32px}
  .panel{padding:22px}
  .card .value{font-size:26px}
  .chart-scroll svg{min-width:560px}
  .toolbar-cluster{position:static;margin:0 0 14px;justify-content:flex-end}
}

@media (max-width:720px){
  .page{padding:20px 18px 20px}
  h1{font-size:28px}
  .section-nav{margin:0 0 22px;padding:6px;gap:4px;flex-wrap:nowrap;white-space:nowrap}
  .panel{padding:18px 16px;border-radius:10px}
  .hero{grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:10px}
  .card{padding:14px 16px}
  .card .value{font-size:24px}
  .bench-table{font-size:12px}
  .bench-table thead th,.bench-table tbody td{padding:9px 10px}
  .legend-map{grid-template-columns:repeat(auto-fit,minmax(220px,1fr))}
  .chart-scroll svg{min-width:520px}
  .recommendation-lead::before{display:none}
  .recommendation-lead{padding-left:14px}
}

@media (max-width:640px){
  .page{padding:16px 14px 16px}
  h1{font-size:25px}
  .report-meta{font-size:12.5px;gap:4px 12px}
  .section-nav{position:relative;top:auto;margin:0 0 18px;padding:5px;gap:4px;flex-wrap:nowrap;white-space:nowrap;-webkit-mask-image:linear-gradient(to right,#000 92%,transparent);mask-image:linear-gradient(to right,#000 92%,transparent);border-radius:14px}
  .section-nav a{padding:7px 12px;font-size:12.5px}
  .panel{padding:16px 14px;border-radius:10px}
  .panel-head{gap:8px}
  h2{font-size:18px;margin:0 0 12px}
  h2 .h-anchor{width:22px;height:22px;font-size:10px}
  .note{font-size:13px}
  .config-cell{min-width:0}
  .chart-scroll svg{min-width:480px;height:340px}
  .report-header{padding:24px 18px 22px}
}

@media (max-width:720px){
  .heatmap-cell.score.latency{grid-template-columns:auto 1fr;gap:8px;padding:7px 8px;min-height:74px}
  .heatmap-cell.score.latency .lat-meters{gap:3px}
  .heatmap-latency-meter{width:8px;min-height:54px}
  .heatmap-cell.score.latency .lat-row strong{font-size:11.5px}
  .heatmap-cell.score.latency .heatmap-num small{font-size:9px}
  .profile-head{gap:6px}
  .decision-panel .score-sum{font-size:14px;padding:5px 12px;min-width:64px}
  .decision-panel .score-sum .score-sum-max{font-size:10px}
  .merged-h3{font-size:15.5px;padding:8px 12px}
}

@media (max-width:520px){
  body{font-size:14px}
  .page{padding:14px 12px 16px}
  h1{font-size:22px}
  .hero{grid-template-columns:repeat(2,minmax(0,1fr));gap:8px}
  .card{padding:12px 14px;border-radius:10px}
  .card .label{font-size:10px}
  .card .value{font-size:20px}
  .panel{padding:14px 12px;margin-bottom:14px}
  h2{font-size:17px}
  h2 .h-anchor{width:30px;height:30px;font-size:12px;border-radius:8px}
  .note{font-size:12.5px;line-height:1.55}
  .bench-table{font-size:12px}
  .bench-table thead th,.bench-table tbody td{padding:8px 10px}
  .bench-table.cardable{display:block;border:none;font-size:13px}
  .bench-table.cardable thead{display:none}
  .bench-table.cardable tbody{display:block}
  .bench-table.cardable tbody tr{display:block;margin:0 0 10px;border:1px solid var(--border);border-radius:10px;padding:8px 12px;background:var(--surface);box-shadow:var(--shadow-sm)}
  .bench-table.cardable tbody tr:hover td{background:transparent}
  .bench-table.cardable tbody tr:nth-child(even) td{background:transparent}
  .bench-table.cardable tbody td{display:flex;justify-content:space-between;align-items:flex-start;gap:14px;padding:7px 0;border:none;border-bottom:1px dashed var(--border);text-align:right;box-shadow:none;white-space:normal;overflow-wrap:anywhere;word-break:break-word}
  .bench-table.cardable tbody td code{white-space:normal;overflow-wrap:anywhere}
  .bench-table.cardable tbody td:last-child{border-bottom:none}
  .bench-table.cardable tbody td::before{content:attr(data-label);color:var(--text-subtle);font-size:10.5px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;text-align:left;flex:0 0 38%;min-width:0;overflow-wrap:anywhere}
  .bench-table.cardable tbody td.config-cell{flex-direction:column;align-items:stretch;text-align:left}
  .bench-table.cardable tbody td.config-cell::before{flex:none;margin-bottom:6px}
  .bench-table.cardable tbody td.ok,
  .bench-table.cardable tbody td.warn,
  .bench-table.cardable tbody td.fail{box-shadow:none;border-left:none}
  .bench-table.cardable tbody td.ok::after{content:"";width:8px;height:8px;border-radius:999px;background:var(--ok);align-self:center}
  .bench-table.cardable tbody td.warn::after{content:"";width:8px;height:8px;border-radius:999px;background:var(--warn);align-self:center}
  .bench-table.cardable tbody td.fail::after{content:"";width:8px;height:8px;border-radius:999px;background:var(--fail);align-self:center}
  .chart-scroll svg{min-width:440px;height:300px}
  .legend-map{grid-template-columns:1fr;gap:6px}
  .legend-entry{padding:7px 10px}
  .insight-item{padding:14px 14px 14px 56px;margin:10px 0}
  .insight-item::after{width:30px;height:30px;font-size:13px;left:14px}
  .insight-item:hover{transform:none}
  .insight-item div:first-child{font-size:14px}
  .heatmap-cell.score.latency .lat-row .lat-tag{font-size:9px}
  .heatmap-cell.score.latency .lat-row strong{font-size:11px}
  .heatmap-cell.score.latency .lat-row .lat-unit{font-size:8.5px}
  .summary-pill{padding:5px 10px;font-size:12px}
  .summary-pill .pill-num{font-size:13px}
  .profile-head{gap:6px}
  .decision-panel .profile-card{padding-bottom:4px}
  .decision-panel .score-sum{order:99;flex-basis:100%;font-size:13px;padding:6px 12px;min-width:0;justify-content:flex-start;margin-top:4px}
  .recommendation-lead::before{display:none}
  .recommendation-lead{padding-left:18px}
  .merged-h3{font-size:15px;padding:7px 10px;margin:18px 0 10px}
  .h-anchor{width:30px;height:30px;font-size:12px}
}

@media (max-width:380px){
  body{font-size:13px}
  .page{padding:12px 10px 16px}
  h1{font-size:20px}
  .hero{grid-template-columns:1fr;gap:8px}
  .card .value{font-size:20px}
  .panel{padding:12px 10px}
  h2{font-size:16px}
  h2 .h-anchor{width:26px;height:26px;font-size:11px}
  .bench-table.cardable tbody td::before{flex:0 0 42%;font-size:10.5px}
  .chart-scroll svg{min-width:400px;height:280px}
  .heatmap-cell.score.latency{padding:6px;gap:6px;min-height:68px}
  .heatmap-cell.score.latency .lat-meters{display:none}
  .heatmap-cell.score.latency{grid-template-columns:1fr}
  .summary-pill{font-size:11.5px;padding:4px 9px;gap:6px}
  .summary-strip{gap:6px}
  .insight-item{padding:12px 12px 12px 52px}
  .insight-item::after{width:28px;height:28px;font-size:12px;left:12px}
  .recommendation-lead{font-size:13.5px;padding:13px 14px 13px 14px}
  .decision-panel{padding:18px 14px}
  .decision-panel h2{font-size:18px}
  .merged-h3{font-size:14.5px;padding:6px 10px}
}

@media (hover:none){
  .toggle-button{min-height:44px;padding:10px 16px;font-size:14px}
  .section-nav a{min-height:38px;display:inline-flex;align-items:center;padding:8px 14px}
  .bench-table.cardable tbody tr{padding:10px 12px}
  .seg-btn{min-height:36px}
  .legend-entry:hover{transform:none}
}

@media (max-height:500px) and (orientation:landscape){
  .section-nav{position:static;backdrop-filter:none;-webkit-backdrop-filter:none}
  .panel{scroll-margin-top:8px}
}
)CSS"
    R"CSS(
@page{size:landscape;margin:14mm}
@media print{
  html,body{background:#fff;color:#000}
  .page{max-width:none;padding:0}
  .page::before{display:none}
  .skip-link,.section-nav,.toolbar-cluster,.toggle-button,.chart-toolbar,.chart-hint,.lang-toggle,.legend-map{display:none !important}
  .toggle-panel{display:block !important}
  details{break-inside:avoid}
  details>*{display:revert !important}
  details:not([open])>*:not(summary){display:revert !important}
  .panel{box-shadow:none;border:1px solid #999;border-radius:0;break-inside:avoid;padding:8pt 10pt;margin-bottom:8pt;backdrop-filter:none}
  .panel.decision-panel{border:2px solid #333}
  .table-scroll,.chart-scroll{overflow:visible !important}
  .bench-table{font-size:9pt;table-layout:auto}
  .bench-table thead th{position:static;background:#eee !important;color:#000;border-bottom:2px solid #333}
  .bench-table tbody tr{break-inside:avoid}
  .bench-table tbody tr:nth-child(even) td{background:#fafafa !important}
  .card{box-shadow:none;border:1px solid #999;border-radius:0;break-inside:avoid}
  .card::before{display:none}
  .report-header{background:#fff !important;border:2px solid #333;break-after:auto}
  .report-header::after,.report-header::before{display:none}
  h1,h2,h3{color:#000;break-after:avoid}
  h1 .grad{-webkit-background-clip:initial;background-clip:initial;color:#000;background:none}
  a{color:#000;text-decoration:none}
  body{font-family:Georgia,'Times New Roman',serif;font-size:9.5pt;line-height:1.4}
  .bench-footer{display:none}
  .heatmap-grid{break-inside:avoid}
  .recommendation-lead::before{display:none}
  .recommendation-lead{padding-left:14px}
}
)CSS";

}  // namespace vlink::bench::report
