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

#include <string>
#include <string_view>

#include "./report_html_en_us.h"
#include "./report_html_zh_cn.h"

namespace vlink::bench::report::i18n {

inline std::string render_i18n_runtime_block() {
  std::string block;
  block.reserve(4096);
  block += R"(<script>(function(){window.VLINK_I18N={"en":)";
  block += render_i18n_script_json_en_us();
  block += R"(,"zh-CN":)";
  block += render_i18n_script_json_zh_cn();
  block += "};";
  block +=
      // ---------- snapshot fallbacks before any mutation ----------
      "function snapshot(el){"
      "if(el.dataset.i18nFallback===undefined){"
      "if(el.children.length>0){console.warn('[vlink-bench i18n] non-leaf element has data-i18n:',el);}"
      "el.dataset.i18nFallback=el.textContent;"
      "}"
      "}"
      "function snapshotAttr(el,attr){"
      "var k='i18nFallbackAttr_'+attr.replace(/-/g,'_');"
      "if(el.dataset[k]===undefined){el.dataset[k]=el.getAttribute(attr)||'';}"
      "return k;"
      "}"
      "function applyAttrFor(el,dataKey,attr,dict,fallbackDict,missing){"
      "var id=el.getAttribute(dataKey);if(!id)return;"
      "var fbKey=snapshotAttr(el,attr);"
      "var v;"
      "if(dict&&dict[id]!==undefined){v=dict[id];}"
      "else if(fallbackDict&&fallbackDict[id]!==undefined){v=fallbackDict[id];}"
      "else{v=el.dataset[fbKey];missing.push(id);}"
      "el.setAttribute(attr,v);"
      "}"
      // ---------- apply language ----------
      "function applyLang(lang){"
      "var dict=window.VLINK_I18N[lang]||window.VLINK_I18N['en'];"
      "var fb=window.VLINK_I18N['en'];"
      "var missing=[];"
      "document.documentElement.setAttribute('lang',lang);"
      "document.querySelectorAll('[data-i18n]').forEach(function(el){"
      "snapshot(el);"
      "var k=el.getAttribute('data-i18n');"
      "if(dict[k]!==undefined){el.textContent=dict[k];}"
      "else if(fb[k]!==undefined){el.textContent=fb[k];}"
      "else{el.textContent=el.dataset.i18nFallback;missing.push(k);}"
      "});"
      "document.querySelectorAll('[data-i18n-title]').forEach(function(el){"
      "applyAttrFor(el,'data-i18n-title','title',dict,fb,missing);});"
      "document.querySelectorAll('[data-i18n-aria]').forEach(function(el){"
      "applyAttrFor(el,'data-i18n-aria','aria-label',dict,fb,missing);});"
      "document.querySelectorAll('[data-i18n-show]').forEach(function(el){"
      "applyAttrFor(el,'data-i18n-show','data-show-label',dict,fb,missing);});"
      "document.querySelectorAll('[data-i18n-hide]').forEach(function(el){"
      "applyAttrFor(el,'data-i18n-hide','data-hide-label',dict,fb,missing);});"
      "document.querySelectorAll('[data-toggle-target][data-show-label][data-hide-label]').forEach(function(btn){"
      "var tgt=document.getElementById(btn.getAttribute('data-toggle-target'));"
      "var open=tgt&&tgt.style.display==='block';"
      "btn.textContent=open?btn.getAttribute('data-hide-label'):btn.getAttribute('data-show-label');"
      "});"
      "document.querySelectorAll('[data-i18n-label]').forEach(function(el){"
      "applyAttrFor(el,'data-i18n-label','data-label',dict,fb,missing);});"
      "if(missing.length){console.warn('[vlink-bench i18n] missing '+lang+' translations for keys:',missing);}"
      "try{localStorage.setItem('vlink_bench_lang',lang);}catch(e){}"
      "document.querySelectorAll('[data-lang-button]').forEach(function(b){"
      "var on=b.getAttribute('data-lang-button')===lang;"
      "b.classList.toggle('is-on',on);"
      "b.setAttribute('aria-pressed',on?'true':'false');"
      "});"
      "}"
      "window.vlinkBenchApplyLang=applyLang;"
      // ---------- detect default language ----------
      "function detectLang(){"
      "try{var url=new URL(window.location.href);var qp=url.searchParams.get('lang');"
      "if(qp){if(qp==='en'||qp==='zh'||qp==='zh-CN'){return qp==='zh'?'zh-CN':qp;}}}catch(e){}"
      "try{var s=localStorage.getItem('vlink_bench_lang');if(s)return s;}catch(e){}"
      "var nav=(navigator.language||navigator.userLanguage||'').toLowerCase();"
      "if(nav.indexOf('zh')===0)return 'zh-CN';"
      "return 'en';"
      "}"
      "document.addEventListener('DOMContentLoaded',function(){applyLang(detectLang());});"
      "})();</script>";
  return block;
}

inline std::string render_i18n_toggle_buttons() {
  return reinterpret_cast<const char*>(
      u8R"X(<div class="seg-toggle lang-toggle" role="group" data-i18n-aria="aria_language" aria-label="Language"><button type="button" class="seg-btn" data-lang-button="en" aria-pressed="false" onclick="vlinkBenchApplyLang('en')">EN</button><button type="button" class="seg-btn" data-lang-button="zh-CN" aria-pressed="false" onclick="vlinkBenchApplyLang('zh-CN')">中文</button></div>)X");
}

}  // namespace vlink::bench::report::i18n
