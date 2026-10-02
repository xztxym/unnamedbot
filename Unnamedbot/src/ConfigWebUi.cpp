#include "../include/ConfigWebUi.h"

// ============================================================================
// 配置终端前端：单文档 HTML/CSS/JS。
//
// 设计约束（移植自原 GDI 版本，替换掉其实现方式）：
//   * 配色与信息层级沿用 prototype (4).html —— 深蓝黑底、冷冰蓝描边、
//     等宽字标签与切角按钮；
//   * 中英文分别走各自的字体栈。原实现用 GDI 逐字回退，Cascadia Mono 缺汉字
//     时会挤在一起，这里交给 CSS 字体回退，字宽与基线由排版引擎处理；
//   * 字段表、校验与写库仍然全部在 C++ 侧（单一事实来源），
//     本文件只负责渲染、交互并把结果回传宿主。
//
// 与宿主的消息契约（JSON）：
//   前端 → 宿主  ready / save / reload / restore / close / drag
//                / baseUrlChanged / requestPresets
//   宿主 → 前端  init / state / presetOptions / validation / status
// ============================================================================

namespace UnnamedBot {

    namespace {

        const char kConfigWebUiHtml[] = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="color-scheme" content="dark">
<title>控制终端</title>
<style>
:root{
  /* ---- 色板（与原 GDI 版一致）---- */
  --bg1:#0a1530; --bg0:#060b19;
  --card1:#0e162e; --card0:#090f22;
  --edge:#2b3c5f; --edge2:#5e7ca0; --hair:#17213b;
  --inbg:#070d1e; --seg:#1b2a4e;
  --ghost:#131d38; --primary:#1f2d52;
  --txt:#e8f1ff; --tx2:#a9bde0; --tx3:#738bb6;
  --ice:#a8ceff; --ice-hi:#e0edff; --ice-mid:#86aef2; --acc:#87b2ff;
  --head:#eef4ff; --btntxt:#f2f7ff;
  --fail:#a6b2cc; --failtxt:#bcc8de;

  /* ---- 尺度 ---- */
  --s1:4px; --s2:6px; --s3:8px; --s4:12px; --s5:14px; --s6:16px;
  --s7:22px; --s8:24px; --s9:28px; --s10:26px;
  --f-title:22px; --f-head:15px; --f-body:13px; --f-sm:12px; --f-xs:11px; --f-xxs:10px;
  --hdr-h:104px; --ftr-h:84px; --row-h:40px; --ctl-h:32px;
  --label-w:176px; --note-w:168px;

  --sans:"Segoe UI Variable Text","Segoe UI","Microsoft YaHei UI","Microsoft YaHei",system-ui,sans-serif;
  --mono:"Cascadia Mono",Consolas,"Segoe UI Mono",ui-monospace,monospace;
}

*{box-sizing:border-box}
html,body{height:100%}
body{
  margin:0; overflow:hidden;
  font-family:var(--sans);
  color:var(--txt);
  background:
    radial-gradient(120% 78% at 50% -12%, rgba(134,174,242,.10), transparent 62%),
    repeating-linear-gradient(0deg, rgba(94,124,160,.055) 0 1px, transparent 1px 28px),
    repeating-linear-gradient(90deg, rgba(94,124,160,.055) 0 1px, transparent 1px 28px),
    linear-gradient(180deg, var(--bg1), var(--bg0));
  background-attachment:fixed;
  -webkit-user-select:none; user-select:none;
}
/* 输入框内必须能选中/复制文本（密钥、地址都需要） */
input,textarea{-webkit-user-select:text; user-select:text}
#app{display:flex; flex-direction:column; height:100%}

/* ===================== 顶栏 ===================== */
.hdr{
  flex:0 0 var(--hdr-h); position:relative;
  display:flex; align-items:flex-start;
  padding:var(--s8) var(--s9) 0;
  background:linear-gradient(180deg, rgba(10,21,48,.92), rgba(10,21,48,.35));
  backdrop-filter:blur(2px);
  border-bottom:1px solid var(--hair);
}
.hdr-drag{position:absolute; inset:0 64px 0 0; cursor:default}
.hdr-main{position:relative}
.hdr-title{
  font-size:var(--f-title); font-weight:600; letter-spacing:.14em;
  color:var(--head); line-height:1.1;
}
.hdr-sub{
  margin-top:var(--s5);
  font-family:var(--mono); font-size:var(--f-xs);
  letter-spacing:.22em; color:var(--tx3);
}
.hdr-right{position:relative; margin-left:auto; text-align:right; padding-top:var(--s2)}
.hdr-src{
  font-family:var(--mono); font-size:var(--f-xxs);
  letter-spacing:.18em; color:var(--tx3);
}
.hdr-state{
  margin-top:var(--s4); display:flex; align-items:center; justify-content:flex-end;
  gap:var(--s3); font-size:var(--f-sm); color:var(--tx2);
}
.dot{width:8px;height:8px;background:var(--ice);transform:rotate(45deg);flex:0 0 auto}
.dot.dirty{background:var(--acc)}
.close{
  position:absolute; top:var(--s7); right:var(--s9);
  width:24px;height:24px; padding:0;
  display:grid; place-items:center;
  background:none; border:0; color:var(--tx2); cursor:pointer;
  transition:color .12s ease;
}
.close:hover{color:var(--ice-hi)}
.close:focus-visible{outline:1px solid var(--ice-mid); outline-offset:3px}

/* ===================== 卡片区 ===================== */
.scroll{flex:1 1 auto; overflow-y:auto; overflow-x:hidden; padding:var(--s9) var(--s9) 0}
.scroll::-webkit-scrollbar{width:10px}
.scroll::-webkit-scrollbar-thumb{background:#243356;border:2px solid transparent;background-clip:content-box}
.scroll::-webkit-scrollbar-thumb:hover{background:#33507f;background-clip:content-box}
.scroll::-webkit-scrollbar-track{background:transparent}

.card{
  background:linear-gradient(180deg, var(--card1), var(--card0));
  border:1px solid var(--edge);
  padding:var(--s7) var(--s8);
  margin-bottom:var(--s10,26px);
}
.card:last-child{margin-bottom:var(--s9)}
.card-head{display:flex; align-items:baseline; gap:var(--s4)}
.card-no{
  font-family:var(--mono); font-size:var(--f-sm);
  letter-spacing:.16em; color:var(--ice-mid);
}
.card-title{font-size:var(--f-head); font-weight:600; color:var(--head)}
.card-en{
  font-family:var(--mono); font-size:var(--f-xxs);
  letter-spacing:.2em; color:var(--tx3);
}
.card-sub{margin-top:var(--s2); font-size:var(--f-sm); color:var(--tx3); line-height:1.6}

.rows{margin-top:var(--s5); border-top:1px solid var(--hair)}
.row{
  display:grid;
  grid-template-columns:var(--label-w) minmax(0,1fr) var(--note-w);
  align-items:center; gap:var(--s6);
  min-height:var(--row-h); padding:var(--s3) 0;
  border-bottom:1px solid var(--hair);
}
.row:last-child{border-bottom:0}
.row-preset{grid-template-columns:var(--label-w) minmax(0,1fr)}
.cell-label{display:flex; flex-direction:column; gap:2px; min-width:0}
.lb{font-size:var(--f-body); color:var(--txt)}
.lb-en{
  font-family:var(--mono); font-size:var(--f-xxs);
  letter-spacing:.12em; color:var(--tx3);
}
.cell-ctrl{min-width:0}
.cell-note{
  font-size:var(--f-xs); color:var(--tx3);
  text-align:right; line-height:1.5;
}
.cell-err{
  grid-column:1 / -1; margin-top:var(--s1);
  font-size:var(--f-xs); color:var(--failtxt);
  display:none;
}
.row.invalid .cell-err{display:block}
.row.invalid .field,.row.invalid .seg,.row.invalid .sw,.row.invalid .ms-row{border-color:var(--fail)}

/* 输入控件 */
.field{
  height:var(--ctl-h); width:100%; max-width:100%;
  padding:0 10px;
  background:var(--inbg); color:var(--txt);
  border:1px solid var(--edge); border-radius:0;
  font-family:var(--mono); font-size:var(--f-body);
  transition:border-color .12s ease, box-shadow .12s ease;
}
.field::placeholder{color:#4d6389}
.field:hover{border-color:var(--edge2)}
.field:focus{
  outline:none; border-color:var(--ice-mid);
  box-shadow:inset 0 0 0 1px var(--ice-mid);
}
.field-wrap{display:flex; align-items:center; gap:var(--s3); min-width:0}
.reveal{
  flex:0 0 auto; height:var(--ctl-h);
  padding:0 var(--s4);
  background:var(--ghost); color:var(--tx2);
  border:1px solid var(--edge);
  font-family:var(--sans); font-size:var(--f-xs);
  cursor:pointer; transition:border-color .12s ease,color .12s ease;
}
.reveal:hover{border-color:var(--edge2); color:var(--ice)}

/* 开关 */
.sw{
  position:relative; width:52px; height:26px; padding:0;
  background:var(--inbg); border:1px solid var(--edge);
  cursor:pointer; transition:border-color .12s ease, background .12s ease;
}
.sw .knob{
  position:absolute; top:3px; left:3px; width:18px; height:18px;
  background:#31456e;
  transition:transform .14s ease, background .14s ease;
}
.sw[aria-checked="true"]{background:var(--seg); border-color:var(--ice-mid)}
.sw[aria-checked="true"] .knob{transform:translateX(26px); background:var(--ice)}
.sw:hover{border-color:var(--edge2)}
.sw:focus-visible{outline:1px solid var(--ice-mid); outline-offset:2px}

/* 单选分段 */
.seg{display:inline-flex; border:1px solid var(--edge); background:var(--inbg)}
.seg-b{
  padding:0 var(--s6); height:calc(var(--ctl-h) - 2px);
  background:none; border:0; cursor:pointer;
  color:var(--tx2); font-family:var(--mono); font-size:var(--f-xs);
  letter-spacing:.04em; transition:background .12s ease, color .12s ease;
}
.seg-b + .seg-b{border-left:1px solid var(--edge)}
.seg-b:hover{color:var(--ice)}
.seg-b[aria-checked="true"]{background:var(--seg); color:var(--ice-hi)}
.seg-b:focus-visible{outline:1px solid var(--ice-mid); outline-offset:-2px}

/* 预设网格 */
.pset-grid{
  display:grid; grid-template-columns:repeat(4, minmax(0,1fr));
  gap:var(--s2);
}
.pset{
  height:26px; padding:0 var(--s3);
  background:var(--inbg); border:1px solid var(--edge);
  color:var(--tx2); font-size:var(--f-xs);
  overflow:hidden; text-overflow:ellipsis; white-space:nowrap;
  cursor:pointer; transition:border-color .12s ease,color .12s ease,background .12s ease;
}
.pset:hover{border-color:var(--edge2); color:var(--ice)}
.pset[aria-pressed="true"]{background:var(--seg); border-color:var(--ice-mid); color:var(--ice-hi)}
.pset:focus-visible{outline:1px solid var(--ice-mid); outline-offset:1px}
.pset-empty{font-size:var(--f-xs); color:var(--tx3); font-family:var(--mono)}

/* 多选清单（prompt.enabled）：一行一个文件，深底描边，选中用冰蓝 */
.ms-list{display:grid; grid-template-columns:repeat(2, minmax(0,1fr)); gap:var(--s2)}
.ms-row{
  display:flex; align-items:center; gap:var(--s3);
  height:26px; padding:0 var(--s3);
  background:var(--inbg); border:1px solid var(--edge);
  color:var(--tx2); font-size:var(--f-xs);
  cursor:pointer; overflow:hidden;
  transition:border-color .12s ease,color .12s ease,background .12s ease;
}
.ms-row:hover{border-color:var(--edge2); color:var(--ice)}
.ms-row.on{background:var(--seg); border-color:var(--ice-mid); color:var(--ice-hi)}
.ms-row:focus-within{border-color:var(--ice-mid)}
.ms-row input{flex:0 0 auto; width:12px; height:12px; margin:0; accent-color:var(--ice); cursor:pointer}
.ms-name{flex:0 1 auto; min-width:0; overflow:hidden; text-overflow:ellipsis; white-space:nowrap}
.ms-file{
  flex:0 1 auto; margin-left:auto; min-width:0;
  font-family:var(--mono); font-size:var(--f-xxs); color:var(--tx3);
  overflow:hidden; text-overflow:ellipsis; white-space:nowrap;
}
.ms-row.on .ms-file{color:var(--ice-mid)}

/* ===================== 页脚 ===================== */
.ftr{
  flex:0 0 var(--ftr-h);
  display:flex; align-items:center; gap:var(--s6);
  padding:0 var(--s9);
  border-top:1px solid var(--edge);
  background:linear-gradient(180deg, rgba(9,15,34,.6), rgba(6,11,25,.95));
}
.status{
  flex:1 1 auto; min-width:0;
  font-size:var(--f-sm); color:var(--tx3);
  white-space:nowrap; overflow:hidden; text-overflow:ellipsis;
}
.status.ok{color:var(--ice)}
.status.err{color:var(--failtxt)}
.actions{flex:0 0 auto; display:flex; align-items:center; gap:var(--s4)}

/* 切角按钮：外层做描边，内层做底色，两者同形状裁切 */
.btn{
  padding:1px; border:0; background:var(--edge2);
  clip-path:polygon(8px 0,100% 0,100% calc(100% - 8px),calc(100% - 8px) 100%,0 100%,0 8px);
  cursor:pointer;
}
.bi{
  display:grid; place-items:center; height:38px; padding:0 var(--s7);
  background:var(--ghost); color:var(--tx2);
  clip-path:polygon(8px 0,100% 0,100% calc(100% - 8px),calc(100% - 8px) 100%,0 100%,0 8px);
  font-family:var(--sans); font-size:var(--f-body);
  transition:background .12s ease, color .12s ease;
}
.btn:hover{background:var(--ice-hi)}
.btn:hover .bi{background:#18244a; color:var(--ice)}
.btn.primary{background:var(--ice-mid)}
.btn.primary .bi{background:var(--primary); color:var(--btntxt); padding:0 var(--s9)}
.btn.primary:hover{background:var(--ice-hi)}
.btn.primary:hover .bi{background:#26365f}
.btn:focus-visible{outline:1px solid var(--ice-hi); outline-offset:2px}
.btn[disabled]{opacity:.55; cursor:default}

/* 无宿主桥接时的提示（仅在浏览器里直接打开时出现） */
.standalone{
  display:none; margin:0 0 var(--s7);
  padding:var(--s4) var(--s5);
  border:1px solid var(--edge); background:var(--inbg);
  font-size:var(--f-xs); color:var(--failtxt);
}
body.standalone .standalone{display:block}

@media (max-width:820px){
  :root{--label-w:132px; --note-w:0px}
  .row{grid-template-columns:var(--label-w) minmax(0,1fr)}
  .cell-note{display:none}
  .pset-grid{grid-template-columns:repeat(2,minmax(0,1fr))}
  .ms-list{grid-template-columns:minmax(0,1fr)}
}
@media (prefers-reduced-motion:reduce){
  *{transition:none !important}
}
</style>
</head>
<body>
<div id="app">

  <header class="hdr">
    <div class="hdr-drag" id="drag"></div>
    <div class="hdr-main">
      <div class="hdr-title">控制终端</div>
      <div class="hdr-sub">UNNAMEDBOT · CONFIG TERMINAL</div>
    </div>
    <div class="hdr-right">
      <div class="hdr-src">CONFIG SOURCE</div>
      <div class="hdr-state"><span class="dot" id="dot"></span><span id="stateText">已同步 · app.db</span></div>
    </div>
    <button class="close" id="btnClose" title="关闭" aria-label="关闭">
      <svg width="14" height="14" viewBox="0 0 14 14" aria-hidden="true">
        <path d="M1 1 13 13M13 1 1 13" fill="none" stroke="currentColor" stroke-width="1.4"/>
      </svg>
    </button>
  </header>

  <main class="scroll" id="scroll">
    <div class="standalone">未检测到宿主桥接，当前仅预览界面，改动不会写入配置。</div>
    <div id="cards"></div>
  </main>

  <footer class="ftr">
    <div class="status" id="status" role="status" aria-live="polite"></div>
    <div class="actions">
      <button class="btn primary" id="btnSave"><span class="bi">保存</span></button>
      <button class="btn" id="btnReload"><span class="bi">重载</span></button>
      <button class="btn" id="btnRestore"><span class="bi">恢复默认</span></button>
    </div>
  </footer>

</div>

<script>
(function () {
  "use strict";

  var bridge = (window.chrome && window.chrome.webview) ? window.chrome.webview : null;
  if (!bridge) document.body.classList.add("standalone");

  function post(msg) { if (bridge) bridge.postMessage(msg); }

  // ---- 状态 ----
  var sections = [];          // 字段表（宿主下发，C++ 侧是单一事实来源）
  var values = {};            // key -> 原始字符串（当前编辑值）
  var baseline = {};          // key -> 已入库值
  var presets = {};           // key -> [{label,value}]
  var errors = {};            // key -> 校验失败原因
  var revealed = {};          // key -> 密钥是否明文显示

  function esc(s) {
    return String(s === null || s === undefined ? "" : s)
      .replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;").replace(/'/g, "&#39;");
  }

  // 该键是不是某个依赖行的父开关（字段 JSON 里的 requires）：
  // 是的话改它要重画一次，因为依赖行的可见性会跟着变。
  function isDependencySource(key) {
    for (var i = 0; i < sections.length; i++) {
      var f = sections[i].fields;
      for (var j = 0; j < f.length; j++) {
        if (f[j].requires && f[j].requires.key === key) return true;
      }
    }
    return false;
  }

  function isDirty() {
    for (var i = 0; i < sections.length; i++) {
      var f = sections[i].fields;
      for (var j = 0; j < f.length; j++) {
        if (f[j].kind === "preset") continue;   // 预设行只是写入口，不参与脏值判断
        var k = f[j].key;
        if ((values[k] === undefined ? "" : values[k]) !== (baseline[k] === undefined ? "" : baseline[k])) return true;
      }
    }
    return false;
  }

  // ---- 控件 ----
  function controlHtml(f) {
    var v = values[f.key] === undefined ? "" : values[f.key];

    if (f.kind === "bool") {
      var on = (v === "true");
      return '<button class="sw" role="switch" aria-checked="' + (on ? "true" : "false") +
             '" data-key="' + esc(f.key) + '" data-role="bool" aria-label="' + esc(f.label) + '">' +
             '<span class="knob"></span></button>';
    }

    if (f.kind === "enum") {
      var opts = String(f.options || "").split("|").filter(Boolean);
      var html = '<div class="seg" role="radiogroup" aria-label="' + esc(f.label) + '">';
      for (var i = 0; i < opts.length; i++) {
        html += '<button class="seg-b" role="radio" aria-checked="' + (opts[i] === v ? "true" : "false") +
                '" data-key="' + esc(f.key) + '" data-role="enum" data-value="' + esc(opts[i]) + '">' +
                esc(opts[i]) + '</button>';
      }
      return html + '</div>';
    }

    if (f.kind === "preset") {
      var list = presets[f.key] || [];
      if (!list.length) return '<span class="pset-empty">—</span>';
      var target = values[f.presetTarget] === undefined ? "" : values[f.presetTarget];
      var grid = '<div class="pset-grid" role="group" aria-label="' + esc(f.label) + '">';
      for (var n = 0; n < list.length; n++) {
        var active = (list[n].value === target);
        grid += '<button class="pset" aria-pressed="' + (active ? "true" : "false") +
                '" data-key="' + esc(f.key) + '" data-role="preset" data-value="' + esc(list[n].value) +
                '" title="' + esc(list[n].label) + ' · ' + esc(list[n].value) + '">' +
                esc(list[n].label) + '</button>';
      }
      return grid + '</div>';
    }

    if (f.kind === "multi") {
      // 多选清单（prompt.enabled）：值是以逗号分隔的文件名清单（与 PromptLibrary
      // 的存储格式一致），选项走与预设行同一条下发通道 presets[f.key]，
      // label 是显示名、value 是写进配置的文件名。
      var items = presets[f.key] || [];
      if (!items.length) return '<span class="pset-empty">—</span>';
      var chosen = String(v || "").split(",");
      var box = '<div class="ms-list" role="group" aria-label="' + esc(f.label) + '">';
      for (var m = 0; m < items.length; m++) {
        var on = chosen.indexOf(items[m].value) >= 0;
        // 用 <label> 包住 checkbox：点整行即切换，不用自己去算点击区
        box += '<label class="ms-row' + (on ? " on" : "") + '">' +
               '<input type="checkbox" data-key="' + esc(f.key) + '" data-role="multi"' +
               ' data-value="' + esc(items[m].value) + '"' + (on ? " checked" : "") + '>' +
               '<span class="ms-name">' + esc(items[m].label) + '</span>' +
               '<span class="ms-file">' + esc(items[m].value) + '</span></label>';
      }
      return box + '</div>';
    }

    // text / int / double：一律用自由文本，保持与原实现相同的输入自由度，
    // 校验交给宿主（C++）统一裁决。
    var width = f.controlWidth > 0 ? f.controlWidth : 220;
    var type = f.secret && !revealed[f.key] ? "password" : "text";
    var autocomplete = f.secret ? ' autocomplete="off" spellcheck="false"' : ' spellcheck="false"';
    var input = '<input class="field" type="' + type + '" data-key="' + esc(f.key) + '" data-role="input"' +
                ' value="' + esc(v) + '" style="width:' + width + 'px"' +
                ' aria-label="' + esc(f.label) + '"' + autocomplete + '>';
    if (!f.secret) return input;
    return '<div class="field-wrap">' + input +
           '<button class="reveal" data-key="' + esc(f.key) + '" data-role="reveal">' +
           (revealed[f.key] ? "隐藏" : "显示") + '</button></div>';
  }

  function render() {
    var out = "";
    for (var s = 0; s < sections.length; s++) {
      var sec = sections[s];
      out += '<section class="card"><div class="card-head">' +
             '<span class="card-no">' + esc(sec.no) + '</span>' +
             '<span class="card-title">' + esc(sec.title) + '</span>' +
             '<span class="card-en">' + esc(sec.titleEn) + '</span></div>' +
             '<div class="card-sub">' + esc(sec.sub) + '</div><div class="rows">';

      for (var i = 0; i < sec.fields.length; i++) {
        var f = sec.fields[i];
        var bad = !!errors[f.key];
        // 依赖行：父开关不是指定值时整行隐藏。值仍然留在 values 里，
        // 只是用户看不见；保存时由宿主做一致性回写（见 Save()）。
        var held = f.requires && String(values[f.requires.key] === undefined ? "" : values[f.requires.key]) !== f.requires.value;
        out += '<div class="row' + (f.kind === "preset" ? " row-preset" : "") + (bad ? " invalid" : "") + '"' +
               (held ? ' style="display:none"' : '') + '>' +
               '<div class="cell-label"><span class="lb">' + esc(f.label) + '</span>' +
               '<span class="lb-en">' + esc(f.labelEn) + '</span></div>' +
               '<div class="cell-ctrl">' + controlHtml(f) + '</div>';
        if (f.kind !== "preset") {
          out += '<div class="cell-note">' + esc(f.note || "") + '</div>';
        }
        out += '<div class="cell-err">' + esc(errors[f.key] || "") + '</div></div>';
      }
      out += '</div></section>';
    }
    document.getElementById("cards").innerHTML = out;
    updateHeader();
  }

  function updateHeader() {
    var dirty = isDirty();
    document.getElementById("dot").className = dirty ? "dot dirty" : "dot";
    document.getElementById("stateText").textContent = dirty ? "未保存的修改" : "已同步 · app.db";
  }

  function setStatus(st) {
    if (!st) return;
    var el = document.getElementById("status");
    el.textContent = st.text || "";
    el.className = "status" + (st.kind === 1 ? " ok" : (st.kind === 2 ? " err" : ""));
  }

  // ---- 交互 ----
  document.addEventListener("click", function (ev) {
    var el = ev.target.closest("[data-role]");
    if (!el) return;
    var key = el.getAttribute("data-key");
    var role = el.getAttribute("data-role");

    if (role === "bool") {
      values[key] = (values[key] === "true") ? "false" : "true";
      // 父开关会改变依赖行的可见性（requires），整页重画一次最省事，
      // 照 reveal 的做法直接 render()。
      if (isDependencySource(key)) { render(); return; }
      el.setAttribute("aria-checked", values[key]);
      updateHeader();
    } else if (role === "enum") {
      values[key] = el.getAttribute("data-value");
      var group = el.parentNode.querySelectorAll('[data-role="enum"]');
      for (var i = 0; i < group.length; i++) {
        group[i].setAttribute("aria-checked", group[i] === el ? "true" : "false");
      }
      updateHeader();
    } else if (role === "preset") {
      var field = null;
      for (var s = 0; s < sections.length && !field; s++) {
        for (var j = 0; j < sections[s].fields.length; j++) {
          if (sections[s].fields[j].key === key) { field = sections[s].fields[j]; break; }
        }
      }
      if (!field) return;
      values[field.presetTarget] = el.getAttribute("data-value");
      delete errors[field.presetTarget];
      render();
      // 服务地址变了，「可用模型」整行要跟着换；反过来也要刷新当前选中态
      post({ type: "presetsChanged", key: key, values: values });
    } else if (role === "reveal") {
      revealed[key] = !revealed[key];
      render();
      var again = document.querySelector('.reveal[data-key="' + CSS.escape(key) + '"]');
      if (again) again.focus();
    }
  });

  document.addEventListener("input", function (ev) {
    var el = ev.target;
    if (!el || el.getAttribute("data-role") !== "input") return;
    var edited = el.getAttribute("data-key");
    values[edited] = el.value;
    // 改动即清除该行上一次的校验结论，避免旧错误一直挂着
    delete errors[edited];
    var row = el.closest(".row");
    if (row) row.classList.remove("invalid");
    updateHeader();
  });

  document.addEventListener("change", function (ev) {
    var el = ev.target;
    if (!el) return;

    // 多选勾选：只改自身值与脏值指示，不整页重画——
    // 这是一列复选框，重画会把滚动位置和被点中的那一行一起抡掉。
    if (el.getAttribute("data-role") === "multi") {
      var mkey = el.getAttribute("data-key");
      if (!mkey) return;
      var host = el.closest(".ms-list");
      var boxes = host ? host.querySelectorAll('input[data-role="multi"]') : [];
      var picked = [];
      // 以整行的勾选状态为准重建值：顺序就是选项顺序，
      // 目录里已经删掉的旧项自然被丢掉。
      for (var b = 0; b < boxes.length; b++) {
        if (boxes[b].checked) picked.push(boxes[b].getAttribute("data-value"));
      }
      values[mkey] = picked.join(",");
      delete errors[mkey];
      var mrow = el.closest(".row");
      if (mrow) mrow.classList.remove("invalid");
      var line = el.closest(".ms-row");
      if (line) { if (el.checked) line.classList.add("on"); else line.classList.remove("on"); }
      updateHeader();
      return;
    }

    if (el.getAttribute("data-role") !== "input") return;
    var key = el.getAttribute("data-key");
    values[key] = el.value;
    // 手改服务地址后需要重算「可用模型」
    if (key === "api.base_url") post({ type: "presetsChanged", key: "api.base_url", values: values });
  });

  document.getElementById("btnSave").addEventListener("click", function () {
    post({ type: "save", values: values });
  });
  document.getElementById("btnReload").addEventListener("click", function () {
    post({ type: "reload" });
  });
  document.getElementById("btnRestore").addEventListener("click", function () {
    post({ type: "restore" });
  });
  document.getElementById("btnClose").addEventListener("click", function () {
    post({ type: "close" });
  });

  // 顶栏拖动：窗口本身无标题栏，交给宿主进入系统移动循环
  var drag = document.getElementById("drag");
  drag.addEventListener("mousedown", function (ev) {
    if (ev.button !== 0) return;
    ev.preventDefault();
    post({ type: "drag" });
  });

  // ---- 宿主消息 ----
  function applyState(msg) {
    if (msg.sections) sections = msg.sections;
    if (msg.values) values = msg.values;
    if (msg.baseline) baseline = msg.baseline;
    if (msg.presets) presets = msg.presets;
    errors = msg.errors || {};
    setStatus(msg.status);
    render();
    // 宿主在「首次进入」和「重新唤起窗口」时会要求回到顶部；
    // #cards 是整块替换的，滚动位置挂在 .scroll 上，需要显式归零。
    if (msg.resetScroll) document.getElementById("scroll").scrollTop = 0;
  }

  bridge && bridge.addEventListener("message", function (ev) {
    var msg = ev.data || {};
    switch (msg.type) {
      case "init":
      case "state":
        applyState(msg);
        break;
      case "presetOptions":
        presets[msg.key] = msg.options || [];
        // 宿主可能顺手纠正过别的字段（例如换服务商后把「默认模型」收敛到该
        // 服务商的模型）：带上 values 时先同步，否则这一行会按旧值渲染，
        // 保存时提交的还是旧值，纠正等于没做。
        if (msg.values) { values = msg.values; updateHeader(); }
        // 只重画受影响的行，避免整页重排打断滚动
        var sel = '.pset-grid [data-key="' + CSS.escape(msg.key) + '"]';
        var host = document.querySelector(sel);
        if (!host) { render(); break; }
        var section = null;
        for (var s = 0; s < sections.length && !section; s++) {
          for (var j = 0; j < sections[s].fields.length; j++) {
            if (sections[s].fields[j].key === msg.key) { section = sections[s].fields[j]; break; }
          }
        }
        if (!section) break;
        var holder = host.closest(".cell-ctrl");
        holder.innerHTML = controlHtml(section);
        break;
      case "validation":
        errors = msg.errors || {};
        if (msg.values) values = msg.values;
        setStatus(msg.status);
        render();
        break;
      case "status":
        setStatus(msg.status);
        break;
      default:
        break;
    }
  });

  if (bridge) {
    post({ type: "ready" });
  } else {
    // 无宿主时给出静态骨架，便于单独预览排版
    sections = [{ no: "01", title: "AI · 接口与鉴权", titleEn: "ENDPOINT", sub: "宿主未连接。", fields: [
      { key: "preview", label: "示例字段", labelEn: "SAMPLE", note: "预览", kind: "text", controlWidth: 320, options: "", secret: false }
    ] }];
    values = { preview: "" }; baseline = {};
    render();
  }
})();
</script>
</body>
</html>
)HTML";

    } // namespace

    const char* ConfigWebUiHtml() {
        return kConfigWebUiHtml;
    }

} // namespace UnnamedBot
