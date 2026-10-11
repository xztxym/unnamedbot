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
<title>桌宠中枢 · Agent Pet Hub</title>
<style>

/* ============ 0. 令牌（docs/00-tokens.md v2） ============ */
:root{
  --blue:#015FCC; --blue-deep:#013A80; --blue-light:#1E8FE8;
  --navy:#031F64; --navy-official:#1D384A;
  --ink-a:#16CFFB; --ink-b:#7DE6FD; --ink-c:#77FEFC;
  --pink:#FD77D9; --white:#fff; --red:#F00; --slate:#5D6A88;
  --tx-hi:#fff; --tx-2:#DCE6F7; --tx-3:#B9C9E6;
  --ok:#7BF3C8; --warn:#FFD166; --err:#FF8A7A;
  --panel:rgba(3,31,100,.72); --panel-hi:rgba(3,31,100,.9);
  --panel-line:rgba(255,255,255,.22);

  --ff-dis:'Arial Narrow','Bahnschrift SemiCondensed','Segoe UI',sans-serif;
  --ff-art:'Franklin Gothic Heavy','Franklin Gothic Demi','Bahnschrift','Segoe UI',sans-serif;
  --ff-jp:'思源黑体 CN','Microsoft YaHei UI','Microsoft YaHei',sans-serif;
  --ff-mono:'Cascadia Mono',Consolas,ui-monospace,monospace;

  --fs-menu-cjk:56px; --fs-ghost:37vh; --fs-desc:26px;
  --fs-h1:32px; --fs-h2:20px; --fs-body:16px; --fs-ui:14px; --fs-meta:12px;

  --s1:4px; --s2:8px; --s3:12px; --s4:16px; --s5:24px; --s6:32px; --s7:48px; --s8:64px;

  --row-h:200px; --row-step:72px; --col-left:672px; --col-left-wide:880px;
  --num-left:-72px; --num-top:-288px; --pill:32px;

  /* 运动：07-motion.md §7.2 */
  --t-tap:90ms; --t-hover:110ms; --t-snap:140ms;
  --t-plate:260ms; --t-scene:520ms; --t-lock:620ms; --t-out:180ms;
  --t-wipe:120ms; --t-slice:18ms; --t-step:25ms;
  --p3-select:100ms; --p3-deselect:0ms;
  --e-out:cubic-bezier(.22,1,.36,1);
  --e-plate:cubic-bezier(.16,1.06,.3,1);
  --e-hard:cubic-bezier(.85,0,.15,1);
  --e-spring:cubic-bezier(.34,1.56,.64,1);
  --e-quad:cubic-bezier(.25,.46,.45,.94);

  --topbar:64px; --botbar:48px; --rail:120px;
  --loading-hold:1250ms; --splash-bloom:0ms;
}
*{box-sizing:border-box;margin:0;padding:0}
html,body{height:100%}
body{background:var(--blue-deep);color:var(--tx-hi);font-family:var(--ff-jp);
  font-size:var(--fs-body);line-height:1.6;-webkit-font-smoothing:antialiased;
  overflow:hidden;user-select:none}
button{font:inherit;color:inherit;background:none;border:0;cursor:pointer}
.mono{font-family:var(--ff-mono)}

/* 8 向描边：让文字在动态蓝场上站得住（考据做法） */
.outline{text-shadow:
  0 2px 0 var(--slate),0 -2px 0 var(--slate),2px 0 0 var(--slate),-2px 0 0 var(--slate),
  -2px -2px 0 var(--slate),2px -2px 0 var(--slate),2px 2px 0 var(--slate),-2px 2px 0 var(--slate),
  0 1px 6px rgba(0,0,0,.5),0 -1px 6px rgba(0,0,0,.5),1px 0 6px rgba(0,0,0,.5),-1px 0 6px rgba(0,0,0,.5)}
.outline-sm{text-shadow:
  0 1px 0 var(--slate),0 -1px 0 var(--slate),1px 0 0 var(--slate),-1px 0 0 var(--slate),
  0 1px 4px rgba(0,0,0,.55),1px 0 4px rgba(0,0,0,.45)}

/* ============ 1. 外壳 ============ */
.wrap{position:fixed;inset:0;display:flex}
.rail{width:var(--rail);flex:none;background:rgba(3,31,100,.55);border-right:1px solid var(--panel-line);
  display:flex;flex-direction:column;padding:var(--s4) var(--s2);gap:2px;z-index:50;
  backdrop-filter:blur(6px)}
.rail .rt{font-family:var(--ff-dis);font-size:var(--fs-meta);letter-spacing:.24em;text-transform:uppercase;
  color:var(--tx-3);padding:var(--s2) var(--s2) var(--s4);border-bottom:1px solid var(--panel-line);margin-bottom:var(--s3)}
.rail button{display:flex;align-items:center;gap:var(--s2);min-height:40px;padding:0 var(--s2);
  font-size:var(--fs-ui);color:var(--tx-2);text-align:left;white-space:nowrap;
  transition:color var(--t-hover) linear,background var(--t-hover) linear}
.rail button .n{font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--tx-3)}
.rail button:hover{color:var(--tx-hi);background:rgba(255,255,255,.1)}
.rail button[aria-current="true"]{color:var(--navy);background:var(--pink);font-weight:700}
.rail button[aria-current="true"] .n{color:rgba(3,31,100,.75)}
.rail .rfoot{margin-top:auto;padding:var(--s3) var(--s2) 0;border-top:1px solid var(--panel-line);
  font-family:var(--ff-mono);font-size:var(--fs-meta);line-height:1.9;color:var(--tx-3)}

.screens{position:relative;flex:1;min-width:0;overflow:hidden}
.view{position:absolute;inset:0;display:flex;flex-direction:column;
  opacity:0;visibility:hidden;pointer-events:none;overflow:hidden}
/* 焦点态：屏幕既要不吃点击，也要能被 elementFromPoint 判定为"不可交互" */
.view.on{opacity:1;visibility:visible;pointer-events:auto}

/* ============ 2. 蓝场 ============ */
.field{position:absolute;inset:0;z-index:0;overflow:hidden;
  background:
    radial-gradient(115% 85% at 14% 82%, var(--blue-light) 0%, transparent 54%),
    radial-gradient(78% 58% at 82% 6%, rgba(111,211,255,.30) 0%, transparent 62%),
    linear-gradient(162deg, var(--blue) 0%, var(--blue-deep) 100%)}
.caustic{position:absolute;inset:-35%;pointer-events:none;mix-blend-mode:screen;
  background:repeating-linear-gradient(72deg,transparent 0 34px,rgba(255,255,255,.55) 34px 39px);
  opacity:.16;animation:caustic 20s linear infinite}
.caustic.b{background:repeating-linear-gradient(105deg,transparent 0 52px,rgba(255,255,255,.5) 52px 56px);
  opacity:.1;animation-duration:32s;animation-direction:reverse}
@keyframes caustic{from{transform:translate3d(0,0,0)}to{transform:translate3d(140px,-90px,0)}}
.shimmer{position:absolute;inset:0;pointer-events:none;opacity:.5;
  background:radial-gradient(56% 42% at 50% 44%,rgba(255,255,255,.1),transparent 72%)}
.char{position:absolute;left:-3%;bottom:0;height:82%;z-index:1;pointer-events:none;
  filter:drop-shadow(0 30px 60px rgba(0,20,60,.6)) saturate(1.05)}
.char.cut{left:16%;height:88%;opacity:.14}
.ghost{position:absolute;left:30%;top:6%;z-index:2;pointer-events:none;
  font-family:var(--ff-dis);font-weight:700;font-style:italic;
  font-size:var(--fs-ghost);line-height:.8;letter-spacing:-.2em;color:#808080;opacity:.16;
  transform:rotate(90deg);transform-origin:0 0}

/* ============ 3. 顶栏 / 底栏 ============ */
.topbar{position:relative;z-index:6;flex:none;height:var(--topbar);display:flex;align-items:center;
  gap:var(--s3);padding:0 var(--s8);border-bottom:1px solid rgba(255,255,255,.16);
  background:linear-gradient(180deg,rgba(1,58,128,.5),transparent)}
.wordmark{width:22px;height:22px;flex:none;background:var(--white);
  clip-path:polygon(0 0,100% 0,100% 52%,52% 100%,0 100%)}
.topbar .wm{font-size:var(--fs-ui);font-weight:700;letter-spacing:.18em}
.topbar .we{font-family:var(--ff-dis);font-size:var(--fs-meta);letter-spacing:.24em;
  text-transform:uppercase;color:var(--tx-3)}
.topbar .sp{flex:1}
.proj{text-align:right;line-height:1.15}
.proj .pn{display:block;font-family:var(--ff-dis);font-weight:700;font-size:22px;letter-spacing:.22em;
  text-transform:lowercase;color:var(--tx-hi)}
.proj .pv{display:block;font-family:var(--ff-mono);font-size:var(--fs-meta);letter-spacing:.16em;
  color:var(--tx-2);margin-top:2px}

.botbar{position:relative;z-index:6;flex:none;height:var(--botbar);display:flex;align-items:center;
  gap:var(--s3);padding:0 var(--s8);border-top:1px solid rgba(255,255,255,.16);
  background:rgba(3,31,100,.5);font-size:var(--fs-ui);color:var(--tx-2)}
.pill{display:inline-flex;align-items:center;justify-content:center;
  width:var(--pill);height:var(--pill);border-radius:50%;background:rgba(3,31,100,.8);
  border:3px solid var(--white);outline:2px solid var(--slate);
  font-family:var(--ff-dis);font-weight:700;font-size:15px;font-style:normal}
.botbar .sp{flex:1}
.botbar .bm{font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--tx-3)}

/* ============ 4. 主菜单（歪斜选项列） ============ */
.stagewrap{position:absolute;inset:0;z-index:5;pointer-events:none}
.col{position:absolute;left:42%;top:46%;transform:translateY(-50%);
  width:640px;height:calc(92px * 4 + 132px);pointer-events:auto}
.opt{position:absolute;left:0;height:var(--row-h);display:flex;align-items:center;
  width:100%;pointer-events:none}
.opt .hit{position:absolute;inset:0;pointer-events:auto;cursor:pointer}
.opt .pair{position:relative;display:flex;align-items:baseline;gap:var(--s4);
  padding-left:var(--s2);white-space:nowrap;transform-origin:0 50%;
  transform:scale(1);transition:none}          /* 取消 = 0ms 瞬切 */
.opt .lab{display:flex;flex-direction:row;align-items:baseline;gap:var(--s3);line-height:1;
  z-index:2;position:relative}
.opt .cjk{font-family:var(--ff-jp);font-weight:700;font-size:var(--fs-menu-cjk);
  letter-spacing:.04em;line-height:1.05}
.opt .en{font-family:var(--ff-art);font-weight:400;font-size:13px;
  letter-spacing:.34em;text-transform:uppercase;line-height:1;
  color:var(--tx-2);opacity:.88;transition:color var(--t-hover) linear;white-space:nowrap}
.opt .en::before{content:"";display:inline-block;width:14px;height:1px;
  background:currentColor;opacity:.55;vertical-align:3px;margin-right:9px}
/* 墨色循环：相邻行不同色 */
.opt:nth-child(3n+1) .cjk{color:var(--ink-c)}
.opt:nth-child(3n+2) .cjk{color:var(--ink-a)}
.opt:nth-child(3n+3) .cjk{color:var(--ink-b)}
/* 选中：变黑 + 100ms 弹簧 */
.opt.sel .pair{transform:scale(1.38);transition:transform var(--p3-select) var(--e-spring)}
.opt.sel .cjk,.opt.sel .en{color:#000}
.opt.sel{z-index:12}
.opt[data-off="1"] .cjk{color:var(--tx-3);opacity:.75}
.opt[data-off="1"] .en{color:var(--tx-3);opacity:.6}

/* ===== 选中装置：变黑 + 粉白双平行四边形色板 + 重叠笔画翻红 =====
   色板是 .pair 的子元素，因此跟随 .pair 的选中缩放，自身不带旋转 ——
   白板这条斜带才能按设计好的角度真正横穿字身。 */
.plates{position:absolute;left:-24px;right:-16px;top:50%;height:104px;
  transform:translateY(calc(-50% - 14px));z-index:1;opacity:0;
  transition:opacity var(--p3-deselect) linear}
.opt.sel .plates{opacity:1}
.plates .white,.plates .pink{position:absolute;left:0;top:0;width:100%;height:100%}
.plates .white{background:var(--white);
  clip-path:polygon(0% 96%, 100% 20%, 100% 62%, 0% 100%)}
.plates .pink{background:var(--pink);left:-16px;top:8px;
  clip-path:polygon(0% 100%, 100% 16%, 100% 58%, 0% 100%);
  animation:platePulse 750ms linear infinite;transform-origin:52px 100%;
  animation-play-state:paused}          /* 只有选中行在呼吸 */
.opt.sel .plates .pink{animation-play-state:running}
@keyframes platePulse{0%,92%{transform:scale(1)}100%{transform:scale(1.05)}}
/* 红色副本：同一斜带裁切，作为覆盖层叠在字上 → 只有与白板重叠的笔画变红 */
.red{position:absolute;left:0;top:0;z-index:3;color:var(--red);pointer-events:none;
  overflow:hidden;opacity:0;
  clip-path:polygon(0% 96%, 100% 20%, 100% 62%, 0% 100%);
  -webkit-clip-path:polygon(0% 96%, 100% 20%, 100% 62%, 0% 100%)}
.opt.sel .red{opacity:1}
/* !important 必须：墨色循环规则 (.opt:nth-child(3n+1) .cjk) 的优先级会把红压回黑 */
.red .cjk{color:var(--red)!important}
.red .en{color:var(--red)!important}

/* 右下角家具 */
.furniture{position:absolute;right:var(--s8);bottom:var(--s7);z-index:7;text-align:right;
  width:min(520px,42vw)}
.furniture .ds{font-family:var(--ff-jp);font-size:var(--fs-desc);font-style:italic;font-weight:700;
  line-height:1.3;word-break:keep-all;overflow-wrap:break-word}
.furniture hr{border:0;height:1px;background:rgba(255,255,255,.66);margin:var(--s4) 0}
.furniture .keys{display:flex;gap:var(--s5);justify-content:flex-end;align-items:center;
  font-family:var(--ff-dis);font-size:calc(var(--fs-desc) / 2);font-style:italic;white-space:nowrap}
.furniture .keys span{display:inline-flex;align-items:center;gap:var(--s2);white-space:nowrap}

/* ============ 5. 二级页面 ============ */
.pagewrap{position:relative;z-index:5;flex:1;min-height:0;display:flex;flex-direction:column;
  padding:var(--s6) var(--s8) var(--s5)}
/* 背景层（.field / 焦散 / 立绘）不得拦截点击 —— 防止它盖住页面内容 */
.field,.field *{pointer-events:none}
.pagewrap,.pagewrap *{pointer-events:auto}
.phead{display:flex;align-items:flex-start;gap:var(--s5);flex:none}

/* ===== 全局返回键 =====
   结构 = 左端箭头板 + 深底文字区 + 右侧键帽，三段各自 clip-path 拼成一体。
   硬约束：**点击必须永远有效**。
   · 不用 hover 才能点的结构，不用长过渡做唯一反馈；
   · 按压位移只有 scale(.985)（约 0.7px），不会把按钮从光标下挪走 ——
     大位移的 :active 会在按钮边缘让 mouseup 落到按钮外，从而根本不出 click；
   · 动作由 data-back 走统一的 goHome()，与转场锁/在飞定时器完全解耦。 */
.back{position:relative;display:inline-flex;align-items:stretch;flex:none;
  height:46px;padding:0;border:0;background:transparent;cursor:pointer;
  font-family:inherit;color:var(--tx-hi);overflow:visible;pointer-events:auto;
  -webkit-tap-highlight-color:transparent;transform-origin:50% 50%;
  transition:transform var(--t-snap) var(--e-out),filter var(--t-hover) linear}
.back .arw{flex:none;width:46px;display:grid;place-items:center;
  background:var(--white);color:var(--navy);
  clip-path:polygon(0 0,calc(100% - 11px) 0,100% 50%,calc(100% - 11px) 100%,0 100%);
  transition:background var(--t-hover) linear,color var(--t-hover) linear}
/* 人字形雪佛龙（用边框画，比 ◀ 字符利落得多） */
.back .arw::before{content:"";width:11px;height:11px;
  border-left:3px solid currentColor;border-bottom:3px solid currentColor;
  transform:rotate(45deg) translate(2px,-2px);
  transition:transform var(--t-hover) var(--e-out)}
.back .tx{display:flex;align-items:center;gap:var(--s2);padding:0 var(--s4);
  background:rgba(1,26,72,.86);color:var(--tx-hi);
  clip-path:polygon(0 0,calc(100% - 11px) 0,100% 50%,calc(100% - 11px) 100%,0 100%);
  font-size:var(--fs-ui);letter-spacing:.12em;white-space:nowrap;
  transition:background var(--t-hover) linear,color var(--t-hover) linear}
.back .tx .en{font-family:var(--ff-dis);font-size:10px;letter-spacing:.3em;
  text-transform:uppercase;color:var(--ink-c);transition:color var(--t-hover) linear}
.back .cap{flex:none;display:grid;place-items:center;width:34px;
  font-family:var(--ff-dis);font-weight:700;font-size:14px;color:var(--tx-2);
  border:1px solid rgba(255,255,255,.5);border-left:0;background:rgba(3,31,100,.5);
  transition:background var(--t-hover) linear,color var(--t-hover) linear,border-color var(--t-hover) linear}
/* 悬停：箭头板转粉、雪佛龙往左顶、整板加辉光 */
.back:hover{filter:drop-shadow(0 0 14px rgba(253,119,217,.65))}
.back:hover .arw{background:var(--pink);color:var(--navy)}
.back:hover .arw::before{transform:rotate(45deg) translate(0,-4px)}
.back:hover .tx{background:rgba(1,26,72,1);color:#fff}
.back:hover .tx .en{color:#fff}
.back:hover .cap{background:var(--pink);color:var(--navy);border-color:var(--pink)}
/* 按下：类驱动 + :active 双保险，立即生效 */
.back:active,.back.is-press{transform:scale(.985)}
.back:active .arw,.back.is-press .arw{background:var(--ink-c);color:var(--navy)}
.back:active .tx,.back.is-press .tx{background:var(--pink);color:var(--navy);font-weight:700}
.back:active .tx .en,.back.is-press .tx .en{color:rgba(3,31,100,.8)}
.back:active .cap,.back.is-press .cap{background:var(--white);color:var(--navy);border-color:var(--white)}
/* 键盘聚焦 */
.back:focus-visible{outline:2px solid var(--ink-c);outline-offset:3px}
.back[disabled]{opacity:.4;cursor:default}
.back.sm{height:38px}
.back.sm .arw{width:38px}
.back.sm .arw::before{width:9px;height:9px;border-width:2px}
.back.sm .cap{width:28px;font-size:12px}

/* 返回时的屏幕级确认：一道蓝光从左往右横扫。
   返回是瞬间切屏的，按钮自身的过渡来不及被看见；这一下同步插入、下一帧就播。 */
.home-sweep{position:fixed;inset:0;z-index:180;pointer-events:none;
  background:linear-gradient(100deg,
    transparent 0%,
    rgba(111,211,255,.42) 38%,
    rgba(255,255,255,.62) 50%,
    rgba(111,211,255,.42) 62%,
    transparent 100%);
  transform:translateX(-100%);
  transition:transform 340ms cubic-bezier(.85,0,.15,1),opacity 340ms linear}
.home-sweep.go{transform:translateX(100%);opacity:0}

.phead h2{font-size:var(--fs-h1);font-weight:700;line-height:1.1;letter-spacing:.02em}
.phead h2 em{display:block;font-family:var(--ff-dis);font-style:normal;font-size:var(--fs-meta);
  font-weight:400;letter-spacing:.3em;text-transform:uppercase;color:var(--tx-3);margin-top:var(--s2)}
.phead .rhs{margin-left:auto;text-align:right}
.phead .rhs .k{font-family:var(--ff-dis);font-size:var(--fs-meta);letter-spacing:.2em;
  text-transform:uppercase;color:var(--tx-3)}
.phead .rhs .v{display:inline-flex;align-items:center;gap:var(--s2);margin-top:var(--s2);
  font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--tx-hi)}
.phead .rhs .v b{font-weight:400}
.dot{width:9px;height:9px;flex:none;background:var(--tx-3)}
.dot.on{background:var(--ok)} .dot.chk{background:var(--warn)} .dot.dn{background:var(--err)}

/* 深色面板：表单可读性的保证 */
.panelbox{background:var(--panel);border:1px solid var(--panel-line);backdrop-filter:blur(8px);
  display:flex;flex-direction:column;gap:var(--s4);align-content:start}
.cols{flex:1;min-height:0;display:grid;grid-template-columns:1fr 1fr;gap:var(--s5);margin-top:var(--s5)}

/* 灯组 */
.lampbar{display:flex;border-top:1px solid var(--panel-line);border-bottom:1px solid var(--panel-line);
  margin-top:var(--s5);flex:none}
.lamp{flex:1;display:flex;align-items:center;justify-content:center;gap:var(--s2);padding:var(--s3);
  font-family:var(--ff-dis);font-size:var(--fs-meta);letter-spacing:.18em;text-transform:uppercase;
  color:var(--tx-3);position:relative}
.lamp+.lamp::before{content:"";position:absolute;left:0;top:26%;bottom:26%;width:1px;background:var(--panel-line)}
.lamp i{width:9px;height:9px;flex:none;background:var(--tx-3);opacity:.45}
.lamp.on{color:var(--tx-hi)}
.lamp.on i{opacity:1;background:var(--ink-c)}
.lamp.on.wk i{background:var(--warn)}
.lamp.on.ok i{background:var(--ok)}
.lamp.on.er i{background:var(--err)}

/* 字段：类名用 .fld —— **不要叫 .field**，蓝场背景层的 position:absolute;inset:0 会套到字段上 */
.fld{display:flex;flex-direction:column;gap:var(--s1);flex:none}
.flabel{display:flex;align-items:baseline;gap:var(--s3);margin-bottom:var(--s2)}
.flabel .no{font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--ink-c)}
.flabel .nm{font-size:var(--fs-ui);letter-spacing:.04em}
.flabel .en{margin-left:auto;font-family:var(--ff-dis);font-size:var(--fs-meta);
  letter-spacing:.2em;text-transform:uppercase;color:var(--tx-3)}
.ctl{display:flex;align-items:center;height:48px;background:var(--panel-hi);
  border:1px solid var(--panel-line);transition:border-color var(--t-hover) linear}
.ctl:hover{border-color:rgba(255,255,255,.4)}
.ctl:focus-within{border-color:var(--white);background:rgba(3,31,100,1)}
.ctl input{flex:1;min-width:0;height:100%;padding:0 var(--s3);background:none;border:0;outline:none;
  font-family:var(--ff-mono);font-size:var(--fs-ui);color:var(--tx-hi);caret-color:var(--ink-c)}
.ctl input::placeholder{color:var(--tx-3)}
.ctl .sx{padding-right:var(--s3);font-family:var(--ff-dis);font-size:var(--fs-meta);
  letter-spacing:.2em;text-transform:uppercase;color:var(--tx-3)}
.dd{display:flex;align-items:center;height:48px;padding:0 var(--s3);background:var(--panel-hi);
  border:1px solid var(--panel-line);cursor:pointer;transition:border-color var(--t-hover) linear}
.dd:hover{border-color:rgba(255,255,255,.4)}
.dd .v{font-family:var(--ff-mono);font-size:var(--fs-ui)}
.dd .cx{margin-left:auto;color:var(--ink-c);font-family:var(--ff-mono);font-size:var(--fs-meta)}
.hint{font-size:var(--fs-meta);color:var(--tx-3);line-height:1.65;margin-top:var(--s2)}

/* 按钮 */
.btn{display:inline-flex;align-items:center;justify-content:center;gap:var(--s2);height:48px;
  padding:0 var(--s6);font-size:var(--fs-ui);letter-spacing:.12em;
  background:rgba(3,31,100,.85);border:2px solid var(--white);color:var(--tx-hi);
  clip-path:polygon(0 0,calc(100% - 14px) 0,100% 14px,100% 100%,14px 100%,0 calc(100% - 14px));
  transition:background var(--t-hover) linear,transform var(--t-tap) var(--e-hard)}
.btn:hover{background:rgba(3,31,100,1)}
.btn:active{transform:scale(.985)}
.btn.primary{background:var(--pink);border-color:var(--pink);color:#2A0620;font-weight:700}
.btn.primary:hover{background:#FFA0E4}
.btn.sm{height:38px;padding:0 var(--s4);font-size:var(--fs-meta);border-width:1px}
.btn.ghost{border-color:var(--panel-line);background:rgba(3,31,100,.5)}
.actrow{display:flex;align-items:center;gap:var(--s4);margin-top:var(--s5)}
.statline{font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--tx-3)}
.statline.ok{color:var(--ok)}

/* ============ 6. 设置：三分区平铺（无切换） ============ */
.setgrid{flex:1;min-height:0;display:block;margin-top:var(--s5);overflow-y:auto;
  padding-right:var(--s2);scrollbar-width:thin}
.setgrid::-webkit-scrollbar{width:6px}
.setgrid::-webkit-scrollbar-thumb{background:rgba(255,255,255,.28)}
.setbody{display:block}
.sec{margin-bottom:var(--s7)}
.sec:last-child{margin-bottom:var(--s4)}
.sec + .sec{padding-top:var(--s6);border-top:1px solid var(--panel-line)}

/* 分区大标题：左侧白柱 + 粉色斜切编号板 + 大号黑字 + 右侧英文副题 */
.shead{display:flex;align-items:center;gap:var(--s3);margin-bottom:var(--s4);
  padding-left:var(--s2);position:relative}
.shead::before{content:"";position:absolute;left:0;top:50%;transform:translateY(-50%);
  width:4px;height:30px;background:var(--white)}
.shead .sno{flex:none;font-family:var(--ff-dis);font-style:italic;font-weight:700;
  font-size:26px;line-height:1;color:#000;
  background:linear-gradient(100deg,var(--pink),#FFA0E4);
  padding:8px 16px 8px 20px;margin-left:10px;
  clip-path:polygon(0 0,calc(100% - 12px) 0,100% 12px,100% 100%,12px 100%,0 calc(100% - 12px));
  box-shadow:0 4px 0 rgba(0,0,0,.28)}
.shead h3{font-size:30px;font-weight:700;letter-spacing:.04em;color:var(--tx-hi);
  line-height:1.1;margin:0}
.shead .sen{margin-left:auto;font-family:var(--ff-dis);font-weight:700;
  font-size:13px;letter-spacing:.34em;text-transform:uppercase;color:var(--ink-c);white-space:nowrap}

.sec .panelbox{display:flex;flex-direction:column;gap:var(--s4);
  clip-path:polygon(0 0,calc(100% - 18px) 0,100% 18px,100% 100%,0 100%)}
.rows{border-top:1px solid var(--panel-line)}
.row{display:flex;align-items:center;gap:var(--s4);min-height:60px;padding:var(--s2) 0;
  border-bottom:1px solid var(--panel-line)}
.row .lb{flex:none;width:196px;font-size:var(--fs-ui)}
.row .lb em{display:block;font-family:var(--ff-dis);font-style:normal;font-size:var(--fs-meta);
  letter-spacing:.18em;text-transform:uppercase;color:var(--tx-3);margin-top:2px}
.row .ctl.grow{flex:1;max-width:230px}
.rangebtn{flex:1;height:28px;position:relative;background:none;border:0;cursor:pointer;min-width:80px;
  touch-action:none}
.rangebtn .track{position:absolute;left:0;right:0;top:13px;height:2px;background:rgba(255,255,255,.28)}
.rangebtn .fill{position:absolute;left:0;top:13px;height:2px;background:var(--ink-c)}
.rangebtn .knob{position:absolute;top:13px;width:12px;height:12px;background:var(--white);
  transform:translate(-50%,-5px) rotate(45deg)}
.row .val{flex:none;width:64px;text-align:right;font-family:var(--ff-mono);font-size:var(--fs-ui)}
.row .note{flex:none;width:210px;text-align:right;font-family:var(--ff-mono);
  font-size:var(--fs-meta);color:var(--tx-3)}
.trow2{display:flex;flex-direction:column;gap:var(--s2);padding-top:var(--s4);margin-top:var(--s4);
  border-top:1px solid var(--panel-line)}
.trow2:first-of-type{border-top:0;margin-top:var(--s2)}
.trow2 label{font-family:var(--ff-mono);font-size:12px;letter-spacing:.08em;color:var(--tx-2)}
.trow2 label em{font-style:normal;font-size:11px;color:var(--tx-3);letter-spacing:.14em;margin-left:8px}
.txbox{width:100%;background:var(--panel-hi);border:1px solid var(--panel-line);color:var(--tx-hi);
  font-family:var(--ff-jp);font-size:var(--fs-ui);line-height:1.7;padding:var(--s3);outline:none;resize:vertical;
  caret-color:var(--ink-c)}
.txbox:focus{border-color:var(--white)}
/* MCP 工具条目：类名用 .titem —— **不要叫 .tool**。
   对话流的工具气泡就是 class="bub tool"，同名会让气泡吃到
   display:flex / cursor:pointer / border（实测踩到，气泡被撑成一行）。 */
.titem{display:flex;align-items:center;gap:var(--s4);min-height:56px;padding:0 var(--s4);
  border:1px solid var(--panel-line);background:var(--panel);margin-bottom:var(--s2);cursor:pointer;
  transition:border-color var(--t-hover) linear,background var(--t-hover) linear}
.titem:hover{border-color:var(--white)}
.titem .bx{width:16px;height:16px;flex:none;border:2px solid var(--white);position:relative}
.titem .bx::after{content:"";position:absolute;left:4px;top:0;width:5px;height:9px;
  border:solid var(--navy);border-width:0 2px 2px 0;transform:rotate(42deg);opacity:0}
.titem.on .bx{background:var(--white)}
.titem.on .bx::after{opacity:1}
.titem .nm{font-family:var(--ff-mono);font-size:var(--fs-ui);color:var(--tx-hi);flex:none;min-width:104px}
.titem .ds{font-size:var(--fs-meta);color:var(--tx-2);flex:1;min-width:0}
.titem .sc{font-family:var(--ff-mono);font-size:10px;letter-spacing:.1em;padding:2px 8px;flex:none;
  border:1px solid var(--panel-line);color:var(--tx-3)}
.titem .sc.ro{color:var(--ok);border-color:rgba(123,243,200,.4)}
.titem .sc.rw{color:var(--warn);border-color:rgba(255,209,102,.4)}
.titem .tg{font-family:var(--ff-dis);font-size:var(--fs-meta);letter-spacing:.2em;flex:none;
  text-transform:uppercase;color:var(--ink-c);min-width:74px;text-align:right}
.titem:not(.on) .tg{color:var(--tx-3)}
.titem:not(.on) .nm,.titem:not(.on) .ds{opacity:.55}

/* 人设卡 / 工具条目 */
.pcard{border:1px solid var(--panel-line);background:rgba(1,26,72,.55);padding:var(--s4);
  display:flex;gap:var(--s4);align-items:flex-start;margin-bottom:var(--s4);
  clip-path:polygon(0 0,calc(100% - 14px) 0,100% 14px,100% 100%,0 100%)}
.pcard .portrait{width:64px;height:64px;flex:none;border:1px solid var(--panel-line);
  background:radial-gradient(circle at 50% 38%,rgba(111,211,255,.35),rgba(3,31,100,.9));
  display:grid;place-items:center;font-family:var(--ff-dis);font-size:10px;letter-spacing:.18em;color:var(--ink-c)}
.pcard .pbody{flex:1;min-width:0;display:flex;flex-direction:column;gap:var(--s3)}
.pcard .pname{display:flex;align-items:baseline;gap:var(--s3)}
.pcard .pname b{font-size:16px;font-weight:700;color:var(--tx-hi);letter-spacing:.06em}
.pcard .pname em{font-style:normal;font-family:var(--ff-dis);font-size:var(--fs-meta);
  letter-spacing:.2em;text-transform:uppercase;color:var(--ink-c)}
.pcard .ptraits{display:flex;gap:var(--s2);flex-wrap:wrap}
.pcard .ptraits span{font-family:var(--ff-mono);font-size:var(--fs-meta);letter-spacing:.08em;
  color:var(--tx-2);border:1px solid var(--panel-line);padding:3px 9px;background:rgba(111,211,255,.07)}
.pcard .pdesc{font-size:var(--fs-meta);color:var(--tx-2);line-height:1.75}

.tgrid{display:flex;flex-direction:column;gap:var(--s2);border-top:0}
.snote{font-size:var(--fs-meta);color:var(--tx-2);line-height:1.85;padding:var(--s3) 0 var(--s4)}
.snote b{color:var(--tx-hi);font-weight:400}

/* ============ 7. 对话工作台 ============ */
.work{flex:1;min-height:0;display:grid;grid-template-columns:320px 1fr;position:relative;
  overflow:hidden;
  border:1px solid var(--panel-line);background:rgba(3,31,100,.6);backdrop-filter:blur(8px);margin-top:var(--s5)}
/* 扫光层：永远绝对定位，绝不参与网格布局（否则会把两个栏目挤错位） */
.work .sweep{position:absolute;left:0;right:0;top:0;height:2px;z-index:9;pointer-events:none;
  background:linear-gradient(90deg,transparent,var(--ink-c),transparent);opacity:0}
.petcol{border-right:1px solid var(--panel-line);display:flex;flex-direction:column;padding:var(--s4);
  gap:var(--s3);min-height:0}
.pcap{display:flex;justify-content:space-between;font-family:var(--ff-dis);font-size:var(--fs-meta);
  letter-spacing:.2em;text-transform:uppercase;color:var(--tx-3)}
.pcap b{font-family:var(--ff-mono);color:var(--ink-c);font-weight:400}
.petstage{position:relative;flex:1;min-height:240px;border:1px solid var(--panel-line);display:flex;
  align-items:flex-end;justify-content:center;overflow:hidden;
  background:radial-gradient(74% 54% at 50% 36%,rgba(111,211,255,.16),transparent 74%),
             rgba(1,26,72,.5)}
.petstage .tag{position:absolute;left:var(--s3);top:var(--s2);font-family:var(--ff-mono);
  font-size:var(--fs-meta);color:var(--ink-c)}
.petstage img{height:100%;width:auto;max-width:100%;object-fit:contain;object-position:50% 100%;
  margin-bottom:-4%;filter:drop-shadow(0 12px 26px rgba(0,0,0,.5))}
.slots{border-top:1px solid var(--panel-line)}
.slot{display:flex;justify-content:space-between;gap:var(--s2);padding:var(--s2) 0;
  font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--tx-3);
  border-bottom:1px solid var(--panel-line)}
.slot b{color:var(--tx-hi);font-weight:400}
.chatcol{display:flex;flex-direction:column;min-height:0}
.log{flex:1;min-height:0;overflow:hidden;padding:var(--s4);display:flex;flex-direction:column;gap:var(--s3)}
.bub{max-width:76%;padding:var(--s3) var(--s4);font-size:var(--fs-ui);line-height:1.7;
  border:1px solid var(--panel-line);background:rgba(1,26,72,.7)}
.bub .who{display:block;font-family:var(--ff-dis);font-size:10px;letter-spacing:.26em;
  text-transform:uppercase;color:var(--tx-3);margin-bottom:var(--s1)}
.bub.me{align-self:flex-end;background:var(--pink);border-color:var(--pink);color:#2A0620;font-weight:600}
.bub.me .who{color:rgba(42,6,32,.7)}
.bub.tool{align-self:center;max-width:92%;font-family:var(--ff-mono);font-size:var(--fs-meta);
  color:var(--ink-c);background:rgba(1,26,72,.85);padding:var(--s1) var(--s3)}
.bub.tool em{color:var(--tx-hi);font-style:normal}
.comp{border-top:1px solid var(--panel-line);padding:var(--s3) var(--s4)}
.paramrow{display:flex;align-items:center;gap:var(--s4);margin-bottom:var(--s3);
  font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--tx-3)}
.paramrow b{color:var(--tx-hi);font-weight:400}
.tog{display:inline-flex;border:1px solid var(--panel-line)}
.tog button{padding:var(--s2) var(--s4);font-family:var(--ff-mono);font-size:var(--fs-meta);
  color:var(--tx-3);transition:background var(--t-hover) linear,color var(--t-hover) linear}
.tog button.on{background:var(--pink);color:#2A0620;font-weight:700}
.sendrow{display:flex;gap:var(--s3)}
.inp{flex:1;min-height:52px;max-height:120px;padding:var(--s3);background:var(--panel-hi);
  border:1px solid var(--panel-line);color:var(--tx-hi);font-family:var(--ff-jp);
  font-size:var(--fs-ui);resize:none;outline:none;caret-color:var(--ink-c)}
.inp:focus{border-color:var(--white)}
.meta{margin-top:var(--s2);font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--tx-3)}

/* ============ 8. 加载屏（真正独立的一屏） ============ */
.loadscreen{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;
  justify-content:center;gap:var(--s4);
  background:
    radial-gradient(80% 60% at 50% 46%, rgba(111,211,255,.30), transparent 68%),
    radial-gradient(120% 90% at 14% 84%, var(--blue-light) 0%, transparent 55%),
    linear-gradient(162deg,#0A63D6 0%,#01418F 58%,#012E6B 100%)}
.loadscreen .lv-t{font-family:var(--ff-mono);font-size:var(--fs-meta);letter-spacing:.42em;color:#CFF0FF}
.lv-bar{width:220px;height:3px;background:rgba(255,255,255,.28);overflow:hidden;
  box-shadow:0 0 12px rgba(160,220,255,.5)}
.lv-bar i{display:block;height:100%;width:100%;transform:scaleX(0);transform-origin:left;
  background:linear-gradient(90deg,#CFF0FF,#6fd3ff);box-shadow:0 0 14px rgba(160,220,255,.95)}
.loadscreen .lv-n{font-family:var(--ff-mono);font-size:var(--fs-meta);color:#BEE2FF;letter-spacing:.2em}
.loadscreen .lv-pct{font-family:var(--ff-mono);font-size:var(--fs-meta);color:#8f9dbe;letter-spacing:.2em;
  margin-top:var(--s1)}
/* 加载屏也有返回入口：任何一屏都能退出来 */
.loadscreen .back{position:absolute;left:var(--s8);top:var(--s6);z-index:2}

/* ============ 9. 覆盖层 / 提示 ============ */
.ov{position:fixed;inset:0;z-index:80;display:grid;place-items:center;visibility:hidden;opacity:0;
  pointer-events:none;background:linear-gradient(162deg,rgba(1,58,128,.97),rgba(1,26,72,.99));
  transition:opacity var(--t-out) linear,visibility var(--t-out)}
.ov.on{visibility:visible;opacity:1;pointer-events:auto}
.ovinner{text-align:center}
.bigcap{font-size:var(--fs-h1);font-weight:700;letter-spacing:.24em;text-align:center}
.smallcap{margin-top:var(--s3);font-family:var(--ff-mono);font-size:var(--fs-meta);
  color:var(--tx-2);text-align:center;letter-spacing:.1em}
.toast{position:fixed;right:var(--s5);bottom:var(--s5);z-index:200;pointer-events:none;
  background:rgba(1,26,72,.96);border:1px solid rgba(255,255,255,.34);color:#fff;
  font-family:var(--ff-mono);font-size:var(--fs-meta);letter-spacing:.1em;padding:10px 16px;
  opacity:0;transition:opacity .2s linear;
  clip-path:polygon(0 0,calc(100% - 10px) 0,100% 10px,100% 100%,10px 100%,0 calc(100% - 10px))}
.toast.on{opacity:1}

/* ============ 10. 转场：6 片切片，连续波浪 ============
   时间账：落下 0→214ms（末片 94+120）→ 214ms 换屏（此刻已全遮）→
   末片先抽走 214→334ms → 438ms 全部清空。 */
.wipe{position:fixed;inset:0;z-index:90;pointer-events:none;visibility:hidden;
  display:grid;grid-template-columns:18% 14% 22% 16% 12% 18%;transition:visibility 0s linear 400ms}
.wipe i{background:linear-gradient(180deg,#BFE0FF 0 1px,#015FCC 55%,#013A80 100%);
  transform:translate3d(0,-101%,0);will-change:transform}
.wipe.active{visibility:visible;transition:none}
.wipe.drop i{animation:sliceDown var(--t-wipe) var(--e-hard) both}
.wipe.drop i:nth-child(1){animation-delay:0ms}
.wipe.drop i:nth-child(2){animation-delay:18ms}
.wipe.drop i:nth-child(3){animation-delay:36ms}
.wipe.drop i:nth-child(4){animation-delay:58ms}
.wipe.drop i:nth-child(5){animation-delay:76ms}
.wipe.drop i:nth-child(6){animation-delay:94ms}
/* 抽走反向错峰：末片先走，一浪推一浪 */
.wipe.retract i{animation:sliceUp var(--t-wipe) var(--e-hard) both}
.wipe.retract i:nth-child(6){animation-delay:0ms}
.wipe.retract i:nth-child(5){animation-delay:20ms}
.wipe.retract i:nth-child(4){animation-delay:42ms}
.wipe.retract i:nth-child(3){animation-delay:62ms}
.wipe.retract i:nth-child(2){animation-delay:84ms}
.wipe.retract i:nth-child(1){animation-delay:104ms}
@keyframes sliceDown{from{transform:translate3d(0,-101%,0)}to{transform:translate3d(0,0,0)}}
@keyframes sliceUp{from{transform:translate3d(0,0,0)}to{transform:translate3d(0,-101%,0)}}
/* 换屏那一帧的蓝闪：把接缝糊掉，同时给"切过去"一个重音 */
.flash{position:fixed;inset:0;z-index:89;pointer-events:none;background:var(--blue);opacity:0}
.flash.on{animation:swapFlash 200ms ease-out both}
@keyframes swapFlash{0%{opacity:.95}100%{opacity:0}}

/* ============ 11. 入场编舞（板块先到 · 文字后到） ============
   关键：**绝不在 .pair 上做 transform 动画** —— .pair 的 transform 是选中态
   scale(1.38) 的载体，入场时若也吃 transform，移除 .anim 的那一帧会瞬间弹回。
   也**不要用 clip-path 做入场**：动画被中断时元素会被裁成零宽，整块直接消失。 */
.view.anim .enc{opacity:0;animation:encIn var(--t-plate) var(--e-plate) both}
.view.anim .pair{opacity:0;animation:encPair var(--t-plate) var(--e-out) both;
  animation-delay:var(--anim-t0,40ms)}
@keyframes encIn{from{opacity:0;transform:translate3d(-18px,0,0)}to{opacity:1;transform:none}}
@keyframes encPair{from{opacity:0}to{opacity:1}}
.view.anim .d1{animation-delay:calc(var(--t-step) * 1 + 30ms)}
.view.anim .d2{animation-delay:calc(var(--t-step) * 2 + 30ms)}
.view.anim .d3{animation-delay:calc(var(--t-step) * 3 + 30ms)}
.view.anim .d4{animation-delay:calc(var(--t-step) * 4 + 30ms)}
.view.anim .d5{animation-delay:calc(var(--t-step) * 5 + 30ms)}
.view.anim .d6{animation-delay:calc(var(--t-step) * 6 + 30ms)}
.view.anim .d7{animation-delay:calc(var(--t-step) * 7 + 30ms)}

/* 水面基线：主菜单是「静止水面」，常驻状态，不是过渡 */
.water{position:absolute;left:0;right:0;top:52%;height:1px;z-index:1;pointer-events:none}
.water::before{content:"";position:absolute;left:0;right:0;top:0;height:1px;
  background:linear-gradient(90deg,transparent,rgba(255,255,255,.22) 34%,rgba(255,255,255,.32) 50%,rgba(255,255,255,.22) 66%,transparent)}
.water .ring{position:absolute;left:50%;top:0;width:180px;height:180px;border-radius:50%;
  border:1px solid rgba(255,255,255,.16);transform:translate(-50%,-50%) scale(.5);opacity:.5}
.water .r2{width:320px;height:320px;border-color:rgba(255,255,255,.09);
  animation:ringBreathe 9s ease-in-out infinite}
.water .r1{animation:ringBreathe 9s ease-in-out infinite reverse}
@keyframes ringBreathe{0%,100%{transform:translate(-50%,-50%) scale(.94);opacity:.32}
  50%{transform:translate(-50%,-50%) scale(1.06);opacity:.6}}
.water .lbl{position:absolute;left:50%;bottom:-34px;transform:translateX(-50%);
  font-family:var(--ff-mono);font-size:var(--fs-meta);letter-spacing:.3em;color:var(--tx-3);
  white-space:nowrap;opacity:.8}

/* ============ 12. 水滴 / 起波（屏幕级图层） ============
   #splash 是 .screens 的直接子节点，**不在任何 .view 里** ——
   放进对话屏的话，对话屏未显示时整层 visibility:hidden，动画等于没播。 */
.splash{position:fixed;inset:0;z-index:85;pointer-events:none;opacity:0;
  transition:opacity 200ms linear}
.splash b{position:absolute;top:52%;left:50%;width:2px;height:34px;border-radius:50%;
  background:linear-gradient(180deg,rgba(255,255,255,0),rgba(255,255,255,.95));
  transform:translate(-50%,-72px);opacity:0;transform-origin:50% 100%}
.splash u{position:absolute;left:50%;top:52%;width:70px;height:70px;border-radius:50%;
  transform:translate(-50%,-50%) scale(.2);opacity:0;
  background:radial-gradient(circle,rgba(255,255,255,.85),transparent 68%)}
.splash i{position:absolute;left:50%;top:52%;width:90px;height:90px;
  border:2px solid rgba(255,255,255,.9);border-radius:50%;
  transform:translate(-50%,-50%) scale(.02);opacity:0}
.splash .bloom{position:absolute;left:50%;top:52%;z-index:72;pointer-events:none;
  width:340px;height:340px;border-radius:50%;border:3px solid rgba(255,255,255,.92);
  transform:translate(-50%,-50%) scale(.05);opacity:0}
.splash .bloom.b2{border-width:2px;border-color:rgba(255,255,255,.55);width:300px;height:300px}
#splash.ring{opacity:1}
#splash.blooming{opacity:1}
@keyframes dropFall{
  0%{transform:translate3d(0,-72px,0) scaleY(.7);opacity:0}
  22%{opacity:1}
  100%{transform:translate3d(0,0,0) scaleY(1.05);opacity:0}}
@keyframes rippleSmall{
  0%{transform:translate(-50%,-50%) scale(.02);opacity:0}
  20%{opacity:.85}
  100%{transform:translate(-50%,-50%) scale(1);opacity:0}}
@keyframes rippleBig{
  0%{transform:translate(-50%,-50%) scale(.05);opacity:0}
  10%{opacity:.95}
  100%{transform:translate(-50%,-50%) scale(9);opacity:0}}
#splash.ring b{animation:dropFall 300ms var(--e-hard) both}
#splash.ring b:nth-of-type(1){animation-delay:0ms}
#splash.ring b:nth-of-type(2){left:38%;animation-delay:110ms}
#splash.ring b:nth-of-type(3){left:62%;animation-delay:220ms}
#splash.ring i{animation:rippleSmall 460ms var(--e-out) both}
#splash.ring i:nth-of-type(1){animation-delay:150ms}
#splash.ring i:nth-of-type(2){animation-delay:300ms}
#splash.ring u{animation:rippleSmall 520ms var(--e-out) both;animation-delay:150ms}
#splash.blooming .bloom{animation:rippleBig 760ms var(--e-out) both}
#splash.blooming .bloom.b2{animation-delay:150ms}
/* 新页面从波面里"扩"出来 —— 这才是换屏本身 */
@keyframes splashBloom{0%{clip-path:circle(0% at 50% 52%)}100%{clip-path:circle(150% at 50% 52%)}}
@keyframes splashHead{from{opacity:0;transform:translate3d(0,-14px,0)}to{opacity:1;transform:none}}
.view.splash-bloom .pagewrap,
.view.splash-bloom .work{animation:splashBloom 800ms var(--e-plate) both;
  animation-delay:var(--splash-bloom)}
.view.splash-bloom .phead{animation:splashHead 300ms var(--e-out) both;
  animation-delay:calc(var(--splash-bloom) + 180ms)}
.view.splash-bloom .topbar{animation:splashHead 300ms var(--e-out) both;
  animation-delay:calc(var(--splash-bloom) + 130ms)}

/* ============ 13. 分镜 ============ */
.sb{position:relative;z-index:5;flex:1;min-height:0;overflow-y:auto;padding:var(--s6) var(--s8)}
.sb h2{font-size:var(--fs-h1);font-weight:700}
.sb h2 em{display:block;font-family:var(--ff-dis);font-style:normal;font-size:var(--fs-meta);
  letter-spacing:.3em;text-transform:uppercase;color:var(--tx-3);margin-top:var(--s2)}
.sb .lead{margin-top:var(--s4);color:var(--tx-2);max-width:66ch;font-size:var(--fs-ui);line-height:1.8}
.frames{display:grid;grid-template-columns:repeat(4,1fr);gap:var(--s4);margin-top:var(--s5)}
.frame{border:1px solid var(--panel-line);background:rgba(3,31,100,.6)}
.frame .fr{position:relative;aspect-ratio:16/9;overflow:hidden;
  background:linear-gradient(162deg,var(--blue),var(--blue-deep));display:grid;place-items:center}
.frame .no{position:absolute;left:var(--s2);top:var(--s2);z-index:5;font-family:var(--ff-mono);
  font-size:10px;color:var(--ink-c)}
.frame .pct{position:absolute;right:var(--s2);bottom:var(--s2);z-index:5;font-family:var(--ff-mono);
  font-size:10px;color:var(--tx-3)}
.frame .cap{padding:var(--s3);border-top:1px solid var(--panel-line);font-size:var(--fs-meta);
  color:var(--tx-2);line-height:1.7}
.frame .cap b{display:block;color:var(--tx-hi);margin-bottom:var(--s1);font-size:var(--fs-ui)}
.mini{position:absolute;inset:0}
.mini .r{position:absolute;height:7px;background:var(--ink-a);opacity:.8}
.mini .plate{position:absolute;background:var(--pink);height:14px}
.sb table{width:100%;border-collapse:collapse;margin-top:var(--s4);font-size:var(--fs-ui)}
.sb th,.sb td{border-bottom:1px solid var(--panel-line);padding:var(--s3);text-align:left;vertical-align:top}
.sb th{font-family:var(--ff-dis);font-size:var(--fs-meta);letter-spacing:.2em;text-transform:uppercase;
  color:var(--tx-3);font-weight:400}
.sb td.m{font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--ink-c);white-space:nowrap}
.sb h3{margin-top:var(--s7);font-size:var(--fs-h2)}
.sbdemo{display:flex;gap:var(--s4);margin-top:var(--s5);align-items:center;flex-wrap:wrap}

/* ============ 14. 无障碍 ============ */
:focus-visible{outline:2px solid var(--pink);outline-offset:4px}
@media (prefers-reduced-motion:reduce){
  *{animation-duration:.001ms!important;transition-duration:.001ms!important}
  .wipe{display:none}
}
::-webkit-scrollbar{width:8px;height:8px}
::-webkit-scrollbar-thumb{background:rgba(255,255,255,.3)}
::-webkit-scrollbar-track{background:transparent}


/* ============================================================
   宿主接入层
   ------------------------------------------------------------
   规则只有一条：能用设计稿的组件就用设计稿的组件——
   分区用 .sec/.shead/.sno/.sen，容器用 .panelbox，字段行用
   .rows/.row + .lb/.ctl/.dd/.tog/.rangebtn/.val/.note（这些样式全部
   来自上面那份设计稿样式表，这里不重新发明）。
   这一层只补设计稿里没有的东西：窗口装饰、对话区、加载屏、
   服务商探测灯、以及「？？？」的令牌替换。
   ============================================================ */

/* ---- 顶栏：拖动带 + 右上角隐藏按钮 ---- */
.topbar .sp{flex:1}
.topbar .hstat{display:flex;align-items:center;gap:var(--s3);margin-right:var(--s5);
  font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--tx-3);letter-spacing:.08em}
.dragzone{position:fixed;top:0;left:var(--rail);right:64px;height:var(--topbar);z-index:22;cursor:default}
.xbtn{position:fixed;top:14px;right:16px;width:30px;height:30px;display:flex;align-items:center;
  justify-content:center;border:1px solid var(--panel-line);background:var(--panel);color:var(--tx-2);
  z-index:24;transition:background var(--t-hover),color var(--t-hover),transform var(--t-tap)}
.xbtn:hover{background:var(--pink);color:#fff;transform:rotate(90deg)}
.dot{width:8px;height:8px;border-radius:50%;background:var(--slate);display:inline-block;
  box-shadow:0 0 0 1px rgba(255,255,255,.25) inset}
.dot.on{background:var(--ok);box-shadow:0 0 8px var(--ok)}
.dot.warn{background:var(--warn);box-shadow:0 0 8px var(--warn)}
.dot.err{background:var(--err);box-shadow:0 0 8px var(--err)}

/* ---- 页面骨架：.pagewrap 用设计稿的版本，这里只补返回条右槽 ---- */
.view .pagewrap{padding:var(--s5) var(--s6) var(--s4)}
.botbar{margin-top:auto}
.phead{flex:none;align-items:center;padding-bottom:var(--s4);
  border-bottom:1px solid var(--panel-line);margin-bottom:var(--s5)}
.phead h2{display:flex;align-items:baseline;gap:var(--s4)}
.phead h2 em{font-family:var(--ff-art);font-size:13px;letter-spacing:.34em;text-transform:uppercase;
  color:var(--tx-3);font-style:normal}
.phead .rhs{margin-left:auto;display:flex;align-items:center;gap:var(--s5)}
.phead .rhs .k{font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--tx-3);letter-spacing:.1em}
.phead .rhs .v{display:flex;align-items:center;gap:var(--s2);font-size:var(--fs-ui)}
.scrollarea{flex:1;min-height:0;overflow-y:auto;padding-right:var(--s2);scrollbar-width:thin}
.scrollarea::-webkit-scrollbar{width:8px}
.scrollarea::-webkit-scrollbar-thumb{background:var(--panel-line)}

/* ---- 字段行：在设计稿 .row 的骨架上补几种行内布局 ---- */
/* .panelbox 是 align-items:center 的纵向 flex：行容器必须自己撑满，
   否则整行只有内容宽度——控件被挤窄、说明文字飘到中间。 */
.panelbox > .rows{align-self:stretch;width:100%}
.rows > .row{width:100%}
.row .lb{display:flex;flex-direction:column;gap:2px;line-height:1.35}
.row.lockedrow{flex-wrap:wrap}
.row .tog{flex:none;white-space:nowrap}
.row .tog button{white-space:nowrap}
.row .lb em{font-family:var(--ff-art);font-size:10px;letter-spacing:.24em;text-transform:uppercase;
  color:var(--tx-3);font-style:normal}
.row .ctl{padding:0 var(--s3);flex:1;min-width:0}
/* 设计稿把 .ctl.grow 卡在 230px（它的 API 屏是两栏，230 正好）；
   这里的字段行是整行宽，放开一点，但也不让它拉成一整条。 */
.row .ctl.grow{flex:1;max-width:520px}
.row .dd{flex:1;max-width:420px;min-width:200px}
.rangebtn{max-width:640px}
.row .ctl input{flex:1;min-width:0;background:none;border:0;outline:0;color:var(--tx-hi);
  font-family:var(--ff-mono);font-size:var(--fs-ui);height:100%}
.row .ctl input::placeholder{color:var(--tx-3)}
.row .ctl.grow{flex:1}
.row .note.err{color:var(--err)}
.row .note.ok{color:var(--ink-c)}
/* 值列可以就地编辑：外观仍是设计稿的 .val（等宽右对齐），只是多了一个输入框 */
.row .val input{width:100%;background:none;border:0;outline:0;color:inherit;font:inherit;text-align:right}
.row .val input:focus{color:var(--ink-c)}
.row .val.wide{width:104px}
.row .val{width:76px}

/* 预设/多选：设计稿用 .dd 轮换，模型可能有几十个，逐格点更实际 */
.pset-grid{display:flex;flex-wrap:wrap;gap:var(--s3);flex:1;min-width:0}
.pset{font-family:var(--ff-mono);font-size:var(--fs-meta);padding:var(--s2) var(--s3);
  border:1px solid var(--panel-line);background:rgba(1,26,72,.6);color:var(--tx-2);
  transition:background var(--t-hover),border-color var(--t-hover),color var(--t-hover)}
.pset:hover{background:var(--panel-hi);color:var(--tx-hi)}
.pset[aria-pressed="true"]{border-color:var(--pink);color:#fff;background:rgba(253,119,217,.16)}
.ms{display:flex;flex-wrap:wrap;gap:var(--s3);flex:1;min-width:0}
.ms label{display:flex;align-items:center;gap:var(--s2);font-size:var(--fs-meta);
  padding:var(--s2) var(--s3);border:1px solid var(--panel-line);background:rgba(1,26,72,.6);cursor:pointer}
.ms input{accent-color:var(--pink)}
.lockhint{font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--pink);flex-basis:100%}
.tiny{font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--tx-3)}

/* 滑块：设计稿 .rangebtn 是展示用的，这里补上拖拽时的视觉反馈 */
.rangebtn{cursor:ew-resize}
.rangebtn:focus-visible{outline:1px solid var(--ink-c)}
.rangebtn:active .knob{background:var(--ink-c)}

/* 连通性灯条：四种状态各占一格，命中的那格点亮 */
.lampbar .lamp{opacity:.35;transition:opacity var(--t-hover),color var(--t-hover)}
.lampbar .lamp i{width:8px;height:8px;border-radius:50%;background:currentColor;display:inline-block}
.lampbar .lamp.hit{opacity:1}
.lampbar .lamp.hit.none{color:var(--slate)}
.lampbar .lamp.hit.busy{color:var(--warn)}
.lampbar .lamp.hit.ok{color:var(--ok)}
.lampbar .lamp.hit.bad{color:var(--err)}
.probeline{display:flex;align-items:center;gap:var(--s4);margin-top:var(--s3);flex-wrap:wrap}
.probeline .statline{flex:1;min-width:0;word-break:break-all}

/* ---- 加载屏 ---- */
.loadpct{font-family:var(--ff-art);font-size:56px;line-height:1;color:var(--ink-c);flex:none;width:150px;text-align:right;
  text-shadow:0 0 24px rgba(119,254,252,.35)}
.loadbar{position:relative;flex:1;height:10px;border:1px solid var(--panel-line);background:rgba(1,10,40,.7);overflow:hidden}
.loadbar i{position:absolute;inset:0 100% 0 0;background:linear-gradient(90deg,var(--ink-a),var(--ink-c));
  transition:inset-inline-end var(--t-plate) var(--e-out)}
.loadstep{font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--tx-3)}

/* ---- 对话 ---- */
.chatwrap{flex:1;display:grid;grid-template-columns:320px minmax(0,1fr);gap:var(--s5);min-height:0}
.chatwrap .petcol{border:1px solid var(--panel-line);background:var(--panel);backdrop-filter:blur(8px)}
.chatwrap .pcap{font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--tx-3);letter-spacing:.1em;
  display:flex;justify-content:space-between}
.chatwrap .petstage{flex:1;min-height:120px;position:relative;border:1px solid var(--panel-line);
  background:radial-gradient(80% 60% at 50% 20%,rgba(30,143,232,.25),transparent 70%),rgba(1,26,72,.5);
  display:flex;align-items:center;justify-content:center;overflow:hidden}
.chatwrap .petstage .tag{position:absolute;left:var(--s3);bottom:var(--s3);font-family:var(--ff-mono);
  font-size:10px;color:var(--ink-c);letter-spacing:.14em}
.chatwrap .mdl{font-family:var(--ff-jp);font-size:var(--fs-h2);font-weight:700;text-align:center;padding:var(--s4)}
.chatwrap .slot{display:flex;justify-content:space-between;font-size:var(--fs-meta);
  border-bottom:1px dashed rgba(255,255,255,.08);padding-bottom:2px}
.chatwrap .slot span{color:var(--tx-3)}
.chatwrap .slot b{font-family:var(--ff-mono);font-weight:400}
.chatcol{border:1px solid var(--panel-line);background:var(--panel);backdrop-filter:blur(8px)}
.chatcol .log{overflow-y:auto;padding:var(--s4);gap:var(--s3);scrollbar-width:thin}
.bub{max-width:80%;padding:var(--s3) var(--s4);border:1px solid var(--panel-line);background:rgba(1,26,72,.6);
  font-size:var(--fs-body);line-height:1.7;white-space:pre-wrap;word-break:break-word}
.bub .who{display:block;font-family:var(--ff-art);font-size:10px;letter-spacing:.3em;text-transform:uppercase;
  color:var(--ink-c);margin-bottom:var(--s1)}
.bub.me{align-self:flex-end;border-color:rgba(253,119,217,.5);background:rgba(253,119,217,.1)}
.bub.me .who{color:var(--pink)}
.bub.tool{align-self:flex-start;max-width:92%;font-family:var(--ff-mono);font-size:var(--fs-meta);
  color:var(--tx-2);background:rgba(119,254,252,.07);border-color:rgba(119,254,252,.3)}
.bub.tool em{color:var(--ink-c);font-style:normal;margin-right:var(--s2)}
.bub.sys{align-self:center;font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--tx-3);
  background:transparent;border-style:dashed;max-width:100%}
.bub.stream::after{content:"▌";color:var(--ink-c);animation:blink 900ms steps(1) infinite}
@keyframes blink{50%{opacity:0}}
.comp{border-top:1px solid var(--panel-line);padding:var(--s4);gap:var(--s3)}
.sendrow textarea{flex:1;background:var(--panel-hi);border:1px solid var(--panel-line);color:var(--tx-hi);
  font-family:var(--ff-jp);font-size:var(--fs-body);padding:var(--s3) var(--s4);resize:none;line-height:1.6;min-height:52px}
.sendrow textarea:focus{outline:0;border-color:var(--ink-c)}
.meta{font-family:var(--ff-mono);font-size:var(--fs-meta);color:var(--tx-3);letter-spacing:.04em}

/* ---- 退出确认 ---- */
.overlay{position:fixed;inset:0;background:rgba(1,8,30,.82);backdrop-filter:blur(3px);display:none;
  align-items:center;justify-content:center;z-index:60}
.overlay.on{display:flex}
.ovbox{min-width:440px;background:var(--blue-deep);border:1px solid var(--panel-line);
  box-shadow:0 30px 80px rgba(0,0,0,.6);padding:var(--s7)}
.ovbox h3{font-family:var(--ff-jp);font-size:var(--fs-h1);margin-bottom:var(--s3);display:flex;align-items:baseline;gap:var(--s4)}
.ovbox h3 em{font-family:var(--ff-art);font-size:12px;letter-spacing:.34em;color:var(--tx-3);font-style:normal}
.ovbox p{color:var(--tx-2);font-size:var(--fs-ui);line-height:1.8;margin-bottom:var(--s6)}
.ovbox .acts{display:flex;gap:var(--s4);justify-content:flex-end}

/* ---- 主菜单：会话摘要 + 说明行 ---- */
.homestat{position:absolute;right:var(--s7);top:calc(var(--topbar) + var(--s6));width:320px;
  z-index:3}
.homestat .cap{font-family:var(--ff-art);font-size:11px;letter-spacing:.34em;text-transform:uppercase;color:var(--tx-3)}
.homestat .big{font-family:var(--ff-jp);font-size:var(--fs-h1);font-weight:700;margin:var(--s2) 0 var(--s4);
  word-break:break-all}
.homestat .kv{display:flex;justify-content:space-between;font-size:var(--fs-ui);padding:var(--s2) 0;
  border-bottom:1px dashed rgba(255,255,255,.08)}
.homestat .kv:last-child{border-bottom:0}
.homestat .kv span{color:var(--tx-3)}
.homestat .kv b{font-family:var(--ff-mono);font-weight:400}
.dsline{position:absolute;left:var(--col-left);bottom:calc(var(--botbar) + var(--s6));max-width:560px;
  font-size:var(--fs-ui);color:var(--tx-2);line-height:1.7;z-index:3;min-height:3.4em}
.dsline b{color:var(--ink-c)}
#col .opt.sel .pair{transform:scale(1.06)}

/* ---- 屏幕切换：进场/出场都走同一条过渡，方向由前进/返回决定 ---- */
/* 设计稿本身是「瞬切 + 入场动画」；这里在它外面套一层位移+淡出，
   让两个屏幕在重叠的那 300ms 里看得到彼此的移动方向。
   注意 visibility：它在 .on 上加，但消失要等过渡跑完，否则旧屏会瞬间消失。 */
.view{
  transition:opacity 240ms var(--e-out), transform 360ms var(--e-out),
             filter 300ms var(--e-out), visibility 0s linear 380ms;
  will-change:opacity,transform;
}
.view.on{transition:opacity 240ms var(--e-out), transform 360ms var(--e-out), filter 300ms var(--e-out), visibility 0s linear}
.view.enter-fwd{opacity:0;transform:translate3d(58px,0,0) scale(.992)}
.view.enter-back{opacity:0;transform:translate3d(-58px,0,0) scale(.992)}
.view.leave-fwd{opacity:0;transform:translate3d(-44px,0,0) scale(.992);filter:blur(2px);pointer-events:none}
.view.leave-back{opacity:0;transform:translate3d(44px,0,0) scale(.992);filter:blur(2px);pointer-events:none}
/* 进场时内容分批落位（设计稿的 .enc / .d1..d7 就是干这个的） */
.view.on .pagewrap, .view.on .stagewrap, .view.on .lampbar{animation:screenIn 320ms var(--e-out) both}
@keyframes screenIn{from{opacity:0;transform:translate3d(0,14px,0)}to{opacity:1;transform:none}}
@media (prefers-reduced-motion:reduce){
  .view{transition:none !important}
  .view.on .pagewrap, .view.on .stagewrap, .view.on .lampbar{animation:none !important}
}

/* ---- 前进转场：6 片半透明切片自上落下（设计稿的切片，只收了一档不透明度） ---- */
.slices{position:fixed;inset:0;z-index:70;pointer-events:none;display:none}
.slices.on{display:block}
.slices i{position:absolute;top:-110%;bottom:110%;width:calc(100% / 6);display:block;
  background:linear-gradient(180deg, rgba(6,26,80,.82), rgba(2,10,36,.92));backdrop-filter:blur(1px)}
body.yandere .slices i{background:linear-gradient(180deg, rgba(58,14,28,.82), rgba(24,6,14,.92))}
.slices.on i{animation:sliceIn 300ms linear forwards}
.slices i:nth-child(1){left:0}
.slices i:nth-child(2){left:calc(100% / 6)}
.slices i:nth-child(3){left:calc(200% / 6)}
.slices i:nth-child(4){left:50%}
.slices i:nth-child(5){left:calc(400% / 6)}
.slices i:nth-child(6){left:calc(500% / 6)}
@keyframes sliceIn{
  0%{top:-110%;bottom:110%}
  42%{top:0;bottom:0}
  58%{top:0;bottom:0}
  100%{top:110%;bottom:-110%}
}

/* ---- ？？？：整页换成酒红 + 楷体斜体（只替换令牌，不动结构） ---- */
/* 与宿主 UiTheme::MakeYandereTheme 一一对应。
   上一版是「纯黑底 + 荧光血红」，屏幕上看像报警灯——硬、刺、廉价；
   这一版把红降一档饱和度、底色抬到带紫的深酒红、正文改成暖米白，
   并把设计稿里几处硬编码的青蓝光换成酒色，否则会是「红面板配蓝底」。 */
body.yandere{
  --blue:#8E2A44; --blue-deep:#140A10; --blue-light:#B23A55;
  --navy:#1B0C14; --ink-a:#E27A90; --ink-b:#EFA8B6; --ink-c:#E8A0AE;
  --pink:#D6405C; --ok:#E8A0AE; --warn:#D9A184; --err:#D9635F;
  --tx-hi:#F2E4E6; --tx-2:#D3B3BC; --tx-3:#9C7C86;
  --panel:rgba(46,18,30,.74); --panel-hi:rgba(64,24,40,.92); --panel-line:rgba(226,122,144,.24);
  --ff-jp:'KaiTi','STKaiti','Microsoft YaHei UI',serif;
}
body.yandere .field{
  background:
    radial-gradient(115% 85% at 14% 82%, rgba(190,58,88,.32) 0%, transparent 58%),
    radial-gradient(78% 58% at 82% 6%, rgba(232,138,156,.15) 0%, transparent 62%),
    radial-gradient(120% 90% at 50% 118%, rgba(70,14,34,.5) 0%, transparent 60%),
    linear-gradient(180deg, #1C0A12 0%, #140710 100%);
}
body.yandere .field .caustic{opacity:.45}
body.yandere .shimmer{opacity:.3}
/* 光晕整体收一档：满饱和的硬辉光是「生硬」的主要来源 */
body.yandere .dot.on,body.yandere .dot.warn,body.yandere .dot.err{box-shadow:0 0 6px currentColor}
body.yandere .opt .cjk{font-style:italic;letter-spacing:.08em}
body.yandere .panelbox{box-shadow:inset 0 0 34px rgba(214,64,92,.06)}
body.yandere .bub:not(.me):not(.tool){border-color:rgba(214,64,92,.4);
  box-shadow:inset 0 0 24px rgba(214,64,92,.08)}
body.yandere .water .ring{border-color:rgba(214,64,92,.42)}
/* 血色渐显 + 打字抖动：页面里的输入区也跟上 */
body.yandere .sendrow textarea{border-color:rgba(214,64,92,.42);animation:bloodFade 460ms var(--e-out) both}
@keyframes bloodFade{from{border-color:rgba(74,7,18,.9);box-shadow:none}
  to{border-color:rgba(214,64,92,.42);box-shadow:0 0 0 1px rgba(214,64,92,.18)}}
body.yandere .log .bub:last-child{animation:shakeIn 190ms var(--e-out) both}
@keyframes shakeIn{
  0%{transform:translate(-3px,1px)}
  25%{transform:translate(3px,-1px)}
  50%{transform:translate(-2px,0)}
  100%{transform:none}
}


/* ---- ？？？：把设计稿里硬编码的青蓝逐个换掉 ----
   令牌那一层换完还不够：设计稿的侧栏/顶栏/底栏/面板/气泡/按钮里都写死了海军蓝，
   不覆盖就会变成「红面板配蓝侧栏」的半拉子换肤。另外 .caustic 是白色斜纹
   （mix-blend-mode:screen），在酒色底上会发灰发亮，必须压暗并染成玫瑰色。 */
body.yandere{
  --navy-official:#2A1018; --red:#D6405C; --slate:#7A5A66; --ghost:#2A1018;
  --seg:#3A1622; --inbg:#180A10; --primary:#3A1622;
}
body.yandere .rail{background:rgba(42,16,28,.62)}
body.yandere .rail button[aria-current="true"] .n{color:rgba(52,20,34,.8)}
body.yandere .topbar{background:linear-gradient(180deg, rgba(74,20,38,.46), rgba(30,12,20,.32))}
body.yandere .botbar{background:rgba(42,16,28,.5)}
body.yandere .pill{background:rgba(52,20,34,.8)}
body.yandere .back .tx{background:rgba(58,20,34,.88)}
body.yandere .back:hover .tx{background:rgba(76,26,44,1)}
body.yandere .ctl:focus-within{background:rgba(52,20,34,1)}
body.yandere .btn{background:rgba(46,18,30,.88)}
body.yandere .btn:hover{background:rgba(64,24,40,1)}
body.yandere .pcard{background:rgba(40,16,28,.6)}
body.yandere .pcard .portrait{background:linear-gradient(160deg, rgba(226,122,144,.26), rgba(40,16,28,.9))}
body.yandere .pcard .ptraits span{background:rgba(226,122,144,.08)}
body.yandere .work{background:rgba(40,16,28,.6)}
body.yandere .petstage{background:radial-gradient(80% 60% at 50% 20%, rgba(214,64,92,.22), transparent 70%), rgba(40,16,28,.5)}
body.yandere .bub{background:rgba(40,16,28,.72)}
body.yandere .bub.tool{background:rgba(40,16,28,.86);border-color:rgba(226,122,144,.28)}
body.yandere .ov{background:linear-gradient(180deg, rgba(30,12,20,.99), rgba(74,20,38,.97))}
body.yandere .toast{background:rgba(40,16,28,.96)}
body.yandere .frame{background:rgba(42,16,28,.6)}
body.yandere .caustic{
  background:repeating-linear-gradient(72deg,transparent 0 34px, rgba(255,180,196,.34) 34px 39px);opacity:.085}
body.yandere .caustic.b{
  background:repeating-linear-gradient(105deg,transparent 0 52px, rgba(255,170,190,.3) 52px 56px);opacity:.055}
body.yandere .shimmer{opacity:.32}


/* ---- 低对比度修正 ----
   小字档（--tx-2 / --tx-3）承载的是字段英文名、说明、状态、页脚这些 11–12px 的文字。
   普通主题用的是设计稿自带的浅色档（#DCE6F7 / #B9C9E6），在深色面板上约 8:1；
   但 ？？？ 那一版我自己把它们压暗到了 #D3B3BC / #9C7C86，压到只剩 ~4:1——
   小字在酒色面板上就糊成一片了。这里统一抬回来，并按主题分开处理
   「选中态文字压在自己的色板上」这类会变黑的地方。 */
body.yandere{
  --tx-2:#E6CFD6;      /* 原本 #D3B3BC → 约 9:1 */
  --tx-3:#C9AEB7;      /* 原本 #9C7C86 → 约 6.4:1 */
  --ink-c:#F0AFBC;     /* 等宽小标签用的强调色也提亮一档 */
}
/* 侧栏选中项：设计稿是「深色字压粉板」（普通主题 5.9:1 没问题），
   但 ？？？ 的粉板是酒红，深字只剩 3.5:1 —— 这一档改成浅字。 */
body.yandere .rail button[aria-current="true"]{color:#FFF1F4}
body.yandere .rail button[aria-current="true"] .n{color:rgba(255,241,244,.72)}
/* 分区序号牌：设计稿渐变到硬编码的浅粉，黑字还能看；
   ？？？ 下把渐变整体抬浅，黑字就重新变得扎实。 */
body.yandere .shead .sno{background:linear-gradient(100deg,var(--accentSoft),#F8D3DA)}
/* 状态灯未命中的那几格原来只有 35% 不透明度，太虚，抬到 55% */
.lampbar .lamp{opacity:.55}
/* 虚线分隔在小字底下会削弱可读性，降一档 */
.rows > .row{border-bottom-color:rgba(255,255,255,.06)}
body.yandere .rows > .row{border-bottom-color:rgba(240,175,188,.14)}


/* ---- 选中态的可读性 + 平滑 ----
   设计稿的白色斜带只有 ~40px 高、还带倾斜，而选中文字是纯黑：把「开始」放大看，
   字的上半截落在带子外面，压回深底就是黑字黑底。这里把带子拉成覆盖整行的斜切色板
   （角度保留），黑字就始终落在浅底上；顺带把选中/取消的过渡都做成有时长的，
   原来是「选中 100ms、取消 0ms」——取消那一下是硬切，看着就是不平滑。 */
.opt .plates{transition:opacity 160ms var(--e-out)}
.opt .red{transition:opacity 160ms var(--e-out)}
.opt .pair{transition:transform 140ms var(--e-out)}
.opt.sel .plates .white{clip-path:polygon(0% 84%, 100% 0%, 100% 100%, 0% 100%)}
.opt .plates .white{clip-path:polygon(0% 84%, 100% 0%, 100% 100%, 0% 100%)}
.opt.sel .pair{transition:transform 180ms var(--e-spring)}
/* 侧栏的高亮也跟上：原来背景是瞬切 */
.rail button{transition:color var(--t-hover) linear, background 180ms var(--e-out), transform 180ms var(--e-out)}

/* ---- 退出确认弹层 ----
   两个问题：① 取消按钮是 .btn.ghost，在深色弹层上几乎看不见，
   看起来就像「只有一个退出选项」；② 弹层是 display 直接切换，硬闪。
   这里给取消一个看得见的描边按钮，并让弹层淡入缩放出现、Esc / 点外部关闭。 */
.overlay{
  opacity:0;visibility:hidden;display:flex;
  transition:opacity 180ms var(--e-out), visibility 0s linear 180ms;
  backdrop-filter:blur(0px);
}
.overlay.on{opacity:1;visibility:visible;transition:opacity 200ms var(--e-out), visibility 0s linear, backdrop-filter 200ms}
.ovbox{transform:translateY(14px) scale(.985);transition:transform 200ms var(--e-out)}
.overlay.on .ovbox{transform:none}
.overlay .acts .btn{border-color:var(--panel-line);color:var(--tx-hi);background:var(--panel-hi)}
.overlay .acts .btn:hover{border-color:var(--ink-c);color:#fff}
.overlay .acts .btn.outline{
  background:rgba(255,255,255,.06);border:1px solid rgba(255,255,255,.42);color:var(--tx-hi);
}
.overlay .acts .btn.outline:hover{background:rgba(255,255,255,.14);border-color:var(--ink-c)}
body.yandere .overlay .acts .btn.outline{
  background:rgba(255,214,224,.08);border-color:rgba(246,201,210,.5);color:#FFF1F4;
}
body.yandere .overlay .acts .btn.outline:hover{background:rgba(255,214,224,.18)}
.overlay .acts .btn .cap{
  margin-left:var(--s3);font-family:var(--ff-mono);font-size:10px;letter-spacing:.1em;
  color:var(--tx-3);border:1px solid var(--panel-line);padding:1px 5px;
}
.overlay .acts .btn.outline .cap{border-color:rgba(255,255,255,.3);color:var(--tx-2)}


/* 设计稿里的 .ghost 是主菜单那个巨型幽灵数字（position:absolute + 37vh 字号），
   和按钮变体 .btn.ghost 撞了名：取消按钮一旦用 ghost，就会被那条规则扔出弹层
   （实测 rect 变成 48x150 飘在左上角）。所以按钮的次级样式改叫 .outline。 */
.btn.outline{border-color:var(--panel-line);background:rgba(3,31,100,.5);color:var(--tx-hi)}
.btn.outline:hover{background:var(--panel-hi);border-color:var(--ink-c)}
body.yandere .btn.outline{background:rgba(46,18,30,.6);border-color:rgba(246,201,210,.42)}
body.yandere .btn.outline:hover{background:rgba(64,24,40,.85)}


/* ============================================================
   GUI 动效与交互反馈
   ------------------------------------------------------------
   这里补的是「手感」层：进场落位、悬停/按下反馈、数值变化提示、
   新消息入场。屏幕之间与菜单选中态的过渡在各自的段落里。
   ============================================================ */

/* ---- 按钮：原来 hover 只是把深蓝从 .85 提到 1.0，肉眼几乎看不出；
       现在给抬升 + 描边 + 阴影三件事，按下再收回去。 ---- */
.btn{
  transition:background 160ms var(--e-out), border-color 160ms var(--e-out),
             color 160ms var(--e-out), transform 120ms var(--e-out),
             box-shadow 160ms var(--e-out);
}
.btn:hover{
  transform:translateY(-2px);
  border-color:var(--ink-c);
  box-shadow:0 6px 18px rgba(0,0,0,.32), 0 0 0 1px rgba(119,254,252,.25) inset;
}
.btn:active{transform:translateY(0) scale(.985);box-shadow:none}
.btn:focus-visible{outline:2px solid var(--ink-c);outline-offset:2px}
.btn:disabled{opacity:.45;transform:none;box-shadow:none;cursor:not-allowed}
body.yandere .btn:hover{
  border-color:var(--accentSoft, #E88A9C);
  box-shadow:0 6px 18px rgba(0,0,0,.42), 0 0 0 1px rgba(232,138,156,.28) inset;
}
body.yandere .btn:focus-visible{outline-color:#E88A9C}

/* ---- 进场：分区卡片自上而下依次落位（屏幕级的淡入在 .view 那一段） ---- */
.view.on .sec{animation:cardIn 340ms var(--e-out) both}
.view.on .sec:nth-child(1){animation-delay:20ms}
.view.on .sec:nth-child(2){animation-delay:60ms}
.view.on .sec:nth-child(3){animation-delay:100ms}
.view.on .sec:nth-child(4){animation-delay:140ms}
.view.on .sec:nth-child(5){animation-delay:180ms}
.view.on .sec:nth-child(6){animation-delay:220ms}
.view.on .lampbar{animation:cardIn 300ms var(--e-out) both}
@keyframes cardIn{from{opacity:0;transform:translate3d(0,18px,0)}to{opacity:1;transform:none}}

/* ---- 字段行：悬停时左缘亮一下，长列表里知道自己在哪一行 ---- */
.rows > .row{transition:background 150ms var(--e-out), box-shadow 150ms var(--e-out)}
.rows > .row:hover{
  background:linear-gradient(90deg, rgba(119,254,252,.07), transparent 62%);
  box-shadow:inset 3px 0 0 var(--ink-c);
}
body.yandere .rows > .row:hover{
  background:linear-gradient(90deg, rgba(232,138,156,.10), transparent 62%);
  box-shadow:inset 3px 0 0 var(--accentSoft, #E88A9C);
}

/* ---- 数值变化：滑块拖动时让数字闪一下，不然不知道哪一行在动 ---- */
.row .val.flash, .rv.flash{animation:valFlash 380ms var(--e-out)}
@keyframes valFlash{
  0%{color:var(--ink-c);text-shadow:0 0 14px currentColor}
  100%{color:inherit;text-shadow:none}
}

/* ---- 卡片：轻微抬升（只对非滚动的整体卡片，避免列表里到处乱动） ---- */
.sec .panelbox{transition:box-shadow 200ms var(--e-out), border-color 200ms var(--e-out)}
.sec .panelbox:hover{border-color:rgba(119,254,252,.45)}
body.yandere .sec .panelbox:hover{border-color:rgba(232,138,156,.42)}

/* ---- 开关 / 单选：颜色之外再补上位移，切换更有实体感 ---- */
.tog button{transition:background 140ms var(--e-out), color 140ms var(--e-out), transform 120ms var(--e-out)}
.tog button:not(:disabled):hover{transform:translateY(-1px)}
.tog button:not(:disabled):active{transform:translateY(0) scale(.97)}

/* ---- 记忆库：每条气泡的删除按钮 + 重置按钮 ---- */
.bub{position:relative}
.bub .del{
  position:absolute;top:6px;right:8px;width:20px;height:20px;line-height:18px;padding:0;
  border:1px solid var(--panel-line);background:rgba(3,31,100,.35);color:var(--tx-3);
  border-radius:2px;cursor:pointer;opacity:0;transition:opacity 140ms var(--e-out), color 140ms var(--e-out);
  font-family:var(--ff-mono);font-size:13px;
}
.bub:hover .del{opacity:.75}
.bub .del:hover{opacity:1;color:var(--accent);border-color:var(--accent)}
body.yandere .bub .del{background:rgba(52,20,32,.5)}
body.yandere .bub .del:hover{color:var(--accentSoft, #E88A9C);border-color:var(--accentSoft, #E88A9C)}
button[data-act="chatReset"][data-armed="1"]{border-color:var(--ink-c);color:var(--ink-c)}

/* ---- 对话气泡：新消息入场（只给新增的那一条加 .in，见 renderChat） ---- */
.bub.in{animation:bubIn 260ms var(--e-out) both}
@keyframes bubIn{from{opacity:0;transform:translate3d(0,12px,0)}to{opacity:1;transform:none}}

/* ---- 侧栏当前项：左侧一条随状态呼吸的强调条 ---- */
.rail button[aria-current="true"]{position:relative}
.rail button[aria-current="true"]::before{
  content:"";position:absolute;left:0;top:16%;bottom:16%;width:3px;
  background:var(--ink-c);animation:railPulse 2.6s ease-in-out infinite;
}
@keyframes railPulse{0%,100%{opacity:.5}50%{opacity:1}}

/* ---- 顶栏状态灯：状态变化时闪一下，比只换颜色更容易注意到 ---- */
.dot{transition:background 200ms var(--e-out), box-shadow 200ms var(--e-out)}
.dot.on{animation:dotPop 260ms var(--e-out)}
@keyframes dotPop{0%{transform:scale(.6)}60%{transform:scale(1.25)}100%{transform:scale(1)}}

/* ---- 加载进度：百分比数字跳动时不要抖 ---- */
.loadpct{font-variant-numeric:tabular-nums}

@media (prefers-reduced-motion:reduce){
  .view.on .sec, .view.on .lampbar, .bub.in, .dot.on, .rail button[aria-current="true"]::before{animation:none !important}
  .btn:hover, .rows > .row:hover{transform:none}
}


/* ---- ？？？ 下几处「看得见但费劲」的地方 ----
   ① 选中项的红字重影叠在白色色板上会糊成一片（设计稿的 double exposure）；
   ② 对话里的工具气泡、栏目标签、meta 这类 12px 小字，在酒底上再抬一档；
   ③ 连通性灯条未命中的格子 55% 太虚。 */
body.yandere .opt.sel .red{opacity:.42}
body.yandere .bub.tool{color:var(--tx-hi)}
body.yandere .bub.tool em{color:#F6C9D2}
body.yandere .chatwrap .pcap,
body.yandere .chatwrap .slot span,
body.yandere .meta,
body.yandere .statline,
body.yandere .probeline .statline,
body.yandere .tiny,
body.yandere .row .note,
body.yandere .row .lb em,
body.yandere .loadstep{color:#D9C0C7}
.lampbar .lamp{opacity:.7}
body.yandere .lampbar .lamp{opacity:.75}


/* ---- 适配层里剩下的硬编码深蓝 → 随主题翻转的令牌 ----
   「模型/服务商选择」的候选块、多选方块、加载滑轨、气泡底、弹层遮罩、
   ESC 键帽都还写着固定的海军蓝，？？？ 下会露出一块块蓝。
   这里给适配层自己定义三个底色令牌，普通主题是深蓝、？？？ 是深酒红。 */
:root{
  --host-chip:rgba(1,26,72,.62);   /* 可点击的小块：预设、多选、次级按钮 */
  --host-well:rgba(1,10,40,.72);   /* 凹槽：进度滑轨 */
  --host-veil:rgba(1,8,30,.84);    /* 弹层遮罩 */
}
body.yandere{
  --host-chip:rgba(52,20,32,.62);
  --host-well:rgba(24,8,14,.78);
  --host-veil:rgba(18,6,11,.86);
}
.pset{background:var(--host-chip)}
.ms label{background:var(--host-chip)}
.loadbar{background:var(--host-well)}
.bub{background:var(--host-chip)}
.overlay{background:var(--host-veil)}
.btn.outline{background:var(--host-chip)}
/* ESC 键帽（返回条上的、弹层里的） */
.cap{background:var(--host-chip);border:1px solid var(--panel-line);color:var(--ink-c)}
.back .cap{background:var(--host-chip)}
body.yandere .cap,body.yandere .back .cap{color:#F6C9D2;border-color:rgba(246,201,210,.4)}

/* ---- ？？？ 下的主按钮（测试连接 / 保存）：设计稿是深色字压粉板，
       普通主题的亮粉上没问题（约 7:1），但 ？？？ 的粉板是深酒红，
       深字压上去只剩 ~2:1 —— 看起来就是「深红字压深红底」。这里换浅字并把底色提亮一档。 ---- */
body.yandere .btn.primary{background:#C8385A;border-color:#E88A9C;color:#FFF3F6}
body.yandere .btn.primary:hover{background:#D94A68;border-color:#F6C9D2}
body.yandere .btn.primary:active{background:#B02F4E}

/* ---- ？？？ 下的分区序号牌：设计稿的渐变末尾是硬编码浅粉，
       在酒色场里整体偏暗，黑字压不实。给一块实心浅玫瑰 + 深字（约 7.5:1）。 ---- */
body.yandere .shead .sno{background:#E88A9C;color:#22060C}

/* ---- 主菜单项：指针悬停时也要有反馈 ----
   原来只有「选中」才有视觉变化，如果指针停在某一项上而它恰好不是选中项，
   看起来就是「没反应」。这里给未选中项一个轻微的放大与描边光晕。
   注意用 :not(.sel) —— 否则悬停在选中项上会把它的放大压回去。 */
.opt:not(.sel):hover .pair{transform:scale(1.14)}
.opt:not(.sel):hover .lab .cjk{text-shadow:0 0 20px currentColor}
.opt:not(.sel):hover .en{color:var(--ink-c)}
.opt .hit{cursor:pointer}


/* ---- 主菜单命中区：设计稿的 .hit 是整行 200px 高，而相邻行只隔 92px，
       于是命中区大面积重叠，谁在上面完全由 z-index 决定。
       「刷新」是 z-index:0，被上下两个 z-index:1 的行夹住 →
       指针停在它上面，命中的却是邻居：表现就是「刷新键悬浮/点击没反应」。
       把命中区收成一行的高度（与行距一致，上下不重叠），每一项都能碰到。 */
.opt .hit{inset:auto 0 auto 0;top:50%;height:92px;transform:translateY(-50%)}

</style>
</head>
<body>

<div class="wrap">
  <nav class="rail" aria-label="主导航">
    <div class="rt">HUB</div>
    <button type="button" data-go="home" aria-current="true"><span class="n">01</span>主菜单</button>
    <button type="button" data-go="api"><span class="n">02</span>API 接入</button>
    <button type="button" data-go="settings"><span class="n">03</span>设置</button>
    <button type="button" data-go="chat"><span class="n">04</span>对话</button>
    <button type="button" data-overlay="exit"><span class="n">05</span>退出</button>
    <div class="rfoot">UNNAMEDBOT<br>AGENT HUB<br><span style="color:#FD77D9" id="railBuild">BUILD</span><br>
      <span style="color:#77FEFC">Esc 返回<br>随时可退</span></div>
  </nav>

  <div class="screens" id="screens">

    <!-- ============ 01 主菜单 ============ -->
    <section class="view on" id="v-home" aria-label="主菜单">
      <div class="field">
        <div class="caustic"></div><div class="caustic b"></div><div class="shimmer"></div>
        <div class="ghost" id="ghostNum">01</div>
      </div>

      <header class="topbar">
        <span class="wordmark"></span>
        <span class="wm">桌宠中枢</span><span class="we">Agent Pet Hub</span>
        <span class="sp"></span>
        <span class="hstat"><span class="dot" id="homeDot"></span><b id="homeApi">API 未接入</b></span>
      </header>

      <div class="stagewrap">
        <div class="water" aria-hidden="true">
          <span class="lbl">STILL WATER · 待机</span>
          <span class="ring r1"></span><span class="ring r2"></span>
        </div>

        <div class="col" id="col">
          <div class="opt" data-go-view="chat" data-ds="启动桌宠核心，进入对话工作台"
               style="top:10px;transform:translate(-26px,0) rotate(-5deg);z-index:1">
            <span class="pair">
              <span class="plates"><span class="pink"></span><span class="white"></span></span>
              <span class="lab"><span class="cjk">开始</span><span class="en">Start</span></span>
              <span class="red" aria-hidden="true"><span class="lab"><span class="cjk">开始</span><span class="en">Start</span></span></span>
            </span><span class="hit"></span>
          </div>
          <div class="opt" data-act="refreshPet" data-ds="重读配置、重载模型，把桌宠刷新一遍"
               style="top:102px;transform:translate(0,0) rotate(-3deg);z-index:0">
            <span class="pair">
              <span class="plates"><span class="pink"></span><span class="white"></span></span>
              <span class="lab"><span class="cjk">刷新</span><span class="en">Refresh</span></span>
              <span class="red" aria-hidden="true"><span class="lab"><span class="cjk">刷新</span><span class="en">Refresh</span></span></span>
            </span><span class="hit"></span>
          </div>
          <div class="opt" data-go-view="settings" data-ds="采样、上下文、人设与工具绑定"
               style="top:194px;transform:translate(-16px,0) rotate(-4deg);z-index:1">
            <span class="pair">
              <span class="plates"><span class="pink"></span><span class="white"></span></span>
              <span class="lab"><span class="cjk">设置</span><span class="en">Settings</span></span>
              <span class="red" aria-hidden="true"><span class="lab"><span class="cjk">设置</span><span class="en">Settings</span></span></span>
            </span><span class="hit"></span>
          </div>
          <div class="opt" data-go-view="api" data-ds="服务商、密钥与默认模型"
               style="top:286px;transform:translate(-6px,0) rotate(-3deg);z-index:1">
            <span class="pair">
              <span class="plates"><span class="pink"></span><span class="white"></span></span>
              <span class="lab"><span class="cjk">API 接入</span><span class="en">Endpoint</span></span>
              <span class="red" aria-hidden="true"><span class="lab"><span class="cjk">API 接入</span><span class="en">Endpoint</span></span></span>
            </span><span class="hit"></span>
          </div>
          <div class="opt" data-overlay="exit" data-ds="保存后退出桌宠中枢"
               style="top:378px;transform:translate(-20px,0) rotate(-5deg);z-index:0">
            <span class="pair">
              <span class="plates"><span class="pink"></span><span class="white"></span></span>
              <span class="lab"><span class="cjk">退出</span><span class="en">Quit</span></span>
              <span class="red" aria-hidden="true"><span class="lab"><span class="cjk">退出</span><span class="en">Quit</span></span></span>
            </span><span class="hit"></span>
          </div>
        </div>
      </div>

      <div class="dsline" id="dsline">选一项开始。<b>↑↓</b> 移动 · <b>Enter</b> 确认 · <b>Esc</b> 返回</div>

      <div class="homestat">
        <div class="cap">Session</div>
        <div class="big" id="homeModel">—</div>
        <div class="kv"><span>人设</span><b id="homePersona">关闭</b></div>
        <div class="kv"><span>快速模式</span><b id="homeQuick">—</b></div>
        <div class="kv"><span>上下文</span><b id="homeCtx">—</b></div>
        <div class="kv"><span>工具</span><b id="homeTools">—</b></div>
      </div>

      <footer class="botbar">
        <span class="pill">↑↓</span><span class="outline-sm">选择</span>
        <span style="width:var(--s3)"></span>
        <span class="pill">⏎</span><span class="outline-sm">确认</span>
        <span class="sp"></span>
        <span class="bm" id="homeBm">AGENT HUB · READY</span>
      </footer>
    </section>

    <!-- ============ 00 加载屏 ============ -->
    <section class="view" id="v-loading" aria-label="加载">
      <div class="field"><div class="caustic b"></div><div class="shimmer"></div>
        <div class="ghost" style="left:12%;top:8%;opacity:.12">00</div></div>
      <header class="topbar">
        <span class="wordmark"></span>
        <span class="wm">启动中</span><span class="we">Boot · Loading</span>
        <span class="sp"></span>
        <span class="hstat"><span class="dot warn" id="loadDot"></span><b id="loadPhase">初始化</b></span>
      </header>
      <div class="pagewrap">
        <div class="phead enc">
          <h2>启动中<em>Boot</em></h2>
          <div class="rhs">
            <div class="k">阶段</div>
            <div class="v"><span class="dot warn" id="loadDot2"></span><b id="loadPhase2">初始化</b></div>
          </div>
        </div>
        <section class="sec" style="flex:1;min-height:0;display:flex;flex-direction:column">
          <header class="shead"><span class="sno">00</span><h3>启动序列</h3>
            <span class="sen">Boot Sequence</span></header>
          <div class="panelbox" style="padding:var(--s5);flex:1;min-height:0">
            <div class="rows">
              <div class="row">
                <span class="lb">进度<em>Progress</em></span>
                <span class="loadbar"><i id="loadFill"></i></span>
                <span class="loadpct" id="loadPct">0%</span>
                <span class="note">配置 → 渲染 → 模型</span>
              </div>
              <div class="row">
                <span class="lb">当前步骤<em>Step</em></span>
                <span class="ctl grow"><input type="text" id="loadStep" value="正在读取配置…" readonly></span>
                <span class="val"></span><span class="note">只读</span>
              </div>
            </div>
            <div class="rows" id="loadSteps" style="margin-top:var(--s4)"></div>
          </div>
        </section>
      </div>
      <footer class="botbar">
        <button class="btn outline sm" type="button" data-go="home">跳过</button>
        <span class="sp"></span>
        <span class="bm">BOOT · 首次启动需要编译 Live2D 着色器，约 50 秒</span>
      </footer>
    </section>

    <!-- ============ 02 API 接入 ============ -->
    <section class="view" id="v-api" aria-label="API 接入">
      <div class="field"><div class="caustic b"></div><div class="shimmer"></div>
        <div class="ghost" style="left:10%;top:6%;opacity:.12">02</div></div>
      <header class="topbar">
        <span class="wordmark"></span>
        <span class="wm">API 接入</span><span class="we">Endpoint · Auth</span>
        <span class="sp"></span>
        <span class="hstat"><span class="dot" id="apiDot"></span><b id="apiState">未接入</b></span>
      </header>
      <div class="pagewrap">
        <div class="phead enc">
          <button class="back" type="button" data-back="home"><span class="arw"></span>
            <span class="tx">返回<em class="en">Back</em></span></button>
          <h2>API 接入<em>Connection</em></h2>
          <div class="rhs">
            <div class="k">链路状态</div>
            <div class="v"><span class="dot" id="connDot"></span><b id="connState">未测试</b></div>
          </div>
        </div>

        <!-- 连通性灯条：设计稿的 .lampbar，命中的那一格点亮 -->
        <div class="lampbar enc d1" id="apiLamps">
          <span class="lamp none" data-state="none"><i></i>未测试</span>
          <span class="lamp busy" data-state="busy"><i></i>测试中</span>
          <span class="lamp ok" data-state="ok"><i></i>可达</span>
          <span class="lamp bad" data-state="bad"><i></i>失败</span>
        </div>

        <div class="scrollarea" id="apiCards"></div>
      </div>
      <footer class="botbar">
        <button class="btn primary" type="button" data-act="probe">测试连接</button>
        <span style="width:var(--s4)"></span>
        <span class="statline" id="apiLine">READY · 等待测试</span>
        <span class="sp"></span>
        <span class="statline" id="apiSrc">模型 · 未探测</span>
        <span style="width:var(--s4)"></span>
        <button class="btn sm" type="button" data-act="save">保存</button>
        <span style="width:var(--s3)"></span>
        <button class="btn sm outline" type="button" data-act="reload">重载</button>
      </footer>
    </section>

    <!-- ============ 03 设置 ============ -->
    <section class="view" id="v-settings" aria-label="设置">
      <div class="field"><div class="caustic b"></div><div class="shimmer"></div>
        <div class="ghost" style="left:8%;top:6%;opacity:.12">03</div></div>
      <header class="topbar">
        <span class="wordmark"></span>
        <span class="wm">设置</span><span class="we">Settings · Agent Hub</span>
        <span class="sp"></span>
        <span class="hstat"><span class="dot" id="setDot"></span><b id="setState">未修改</b></span>
      </header>
      <div class="pagewrap">
        <div class="phead enc">
          <button class="back" type="button" data-back="home"><span class="arw"></span>
            <span class="tx">返回<em class="en">Back</em></span></button>
          <h2>设置<em>Settings</em></h2>
          <div class="rhs">
            <div class="k">生效于本机会话</div>
            <div class="v"><span class="dot" id="setDot2"></span><b id="setState2">未修改</b></div>
          </div>
        </div>
        <div class="scrollarea" id="setCards"></div>
      </div>
      <footer class="botbar">
        <button class="btn primary" type="button" data-act="save">保存</button>
        <span style="width:var(--s3)"></span>
        <button class="btn" type="button" data-act="reload">重载</button>
        <span style="width:var(--s3)"></span>
        <button class="btn outline" type="button" data-act="restore">恢复默认</button>
        <span class="sp"></span>
        <span class="bm" id="setStatus"></span>
      </footer>
    </section>

    <!-- ============ 04 对话 ============ -->
    <section class="view" id="v-chat" aria-label="对话">
      <div class="field"><div class="caustic"></div><div class="shimmer"></div>
        <div class="ghost" style="left:14%;top:8%;opacity:.12">04</div></div>
      <header class="topbar">
        <span class="wordmark"></span>
        <span class="wm">对话</span><span class="we">Conversation · Agent Thread</span>
        <span class="sp"></span>
        <span class="hstat"><span class="dot" id="chatDot"></span><b id="chatState">就绪</b></span>
      </header>
      <div class="pagewrap">
        <div class="phead enc">
          <button class="back" type="button" data-back="home"><span class="arw"></span>
            <span class="tx">返回<em class="en">Back</em></span></button>
          <h2>对话<em>Conversation</em></h2>
          <div class="rhs">
            <div class="k">会话</div>
            <div class="v"><span class="dot on" id="sessDot"></span><b id="sessState">已连接</b></div>
          </div>
          <button class="btn sm outline" type="button" data-go="settings">设置</button>
        </div>
        <div class="chatwrap">
          <aside class="petcol">
            <div class="pcap"><span>Live2D · 模型</span><b id="petSlot">Slot 00</b></div>
            <div class="petstage">
              <span class="tag" id="petTag">IDLE</span>
              <div class="mdl" id="petModel">—</div>
            </div>
            <div class="slots">
              <div class="slot"><span>人设</span><b id="petPersona">关闭</b></div>
              <div class="slot"><span>上下文</span><b id="petCtx">—</b></div>
              <div class="slot"><span>工具</span><b id="petTools">—</b></div>
            </div>
          </aside>
          <section class="chatcol">
            <div class="log" id="chatLog">
              <div class="bub sys">等待宿主会话数据…</div>
            </div>
            <div class="comp">
              <div class="paramrow">
                <span>MCP <b id="chatTools">—</b></span>
                <span style="flex:1"></span>
                <button class="btn sm" type="button" data-act="chatReset" title="清空记忆库里的全部对话">重置记忆库</button>
                <span class="tog" id="streamTog" data-tog>
                  <button type="button" class="on" data-stream="1">流式</button>
                  <button type="button" data-stream="0">非流式</button>
                </span>
              </div>
              <div class="sendrow">
                <textarea id="chatInp" rows="2" placeholder="输入消息，Ctrl+Enter 发送"></textarea>
                <button class="btn primary" type="button" data-act="send">发送</button>
              </div>
              <div class="meta" id="chatMeta">SYSTEM ▸ 角色映射已并入 system</div>
            </div>
          </section>
        </div>
      </div>
      <footer class="botbar">
        <span class="pill">⏎</span><span class="outline-sm">换行</span>
        <span style="width:var(--s3)"></span>
        <span class="pill">⌃⏎</span><span class="outline-sm">发送</span>
        <span class="sp"></span>
        <span class="bm" id="chatBm">MODEL —</span>
      </footer>
    </section>

  </div>

  <!-- 全局窗体装饰：顶部拖动带 + 右上角隐藏按钮。
       每个屏幕的顶栏几何一致，所以做成一层，省得逐屏维护。 -->
  <div class="dragzone" data-drag title="拖动窗口"></div>
  <button class="xbtn" type="button" data-act="close" title="隐藏窗口（托盘可再打开）" aria-label="隐藏窗口">
    <svg width="13" height="13" viewBox="0 0 14 14" aria-hidden="true">
      <path d="M1 1 13 13M13 1 1 13" fill="none" stroke="currentColor" stroke-width="1.4"/>
    </svg>
  </button>
</div>

<!-- 退出确认 -->
<div class="overlay" id="ovExit">
  <div class="ovbox">
    <h3>退出桌宠中枢<em>Quit</em></h3>
    <p>未保存的改动会丢失。桌宠本体也会一起退出，托盘图标随之消失。</p>
    <div class="acts">
      <button class="btn outline" type="button" data-overlay-close>取消<span class="cap">ESC</span></button>
      <button class="btn primary" type="button" data-act="quit">退出</button>
    </div>
  </div>
</div>

<!-- 前进转场切片 -->
<div class="slices" id="slices"><i></i><i></i><i></i><i></i><i></i><i></i></div>

<script>
(function () {
  "use strict";

  var bridge = (window.chrome && window.chrome.webview) ? window.chrome.webview : null;
  var MOCK = /(\?|&)mock=1/.test(location.search);
  if (!bridge) document.body.classList.add("standalone");

  function post(msg) { if (bridge) bridge.postMessage(msg); }
  function $(id) { return document.getElementById(id); }
  function esc(s) {
    return String(s === null || s === undefined ? "" : s)
      .replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
      .replace(/"/g, "&quot;").replace(/'/g, "&#39;");
  }

  // ---- 状态 ----
  var sections = [];          // 宿主下发的字段表（唯一真实来源）
  var values = {};            // key -> 当前编辑值（字符串）
  var baseline = {};          // key -> 已保存值
  var presets = {};           // key -> [{label,value}]
  var errors = {};            // key -> 校验失败原因
  var revealed = {};          // key -> 密钥是否明文显示
  var streaming = true;
  var chatMessages = [];
  var current = "home";
  var history = [];           // 屏幕栈，Esc 逐级返回

  // ============================================================
  // 屏幕导航
  // ============================================================
  var SCREENS = { home: 1, loading: 1, api: 1, settings: 1, chat: 1 };
  var RAIL_OF = { home: 0, api: 1, settings: 2, chat: 3 };

  function setRail(screen) {
    var btns = document.querySelectorAll(".rail button[data-go]");
    for (var i = 0; i < btns.length; i++) {
      var on = btns[i].getAttribute("data-go") === screen;
      if (on) btns[i].setAttribute("aria-current", "true");
      else btns[i].removeAttribute("aria-current");
    }
  }

  // 屏幕切换：两个屏幕在重叠的那 ~360ms 里各自向反方向移动并淡出/淡入，
  // 方向由前进/返回决定；前进另外叠一层半透明切片扫过（设计稿的转场）。
  // 单屏内部的入场用 CSS 的 screenIn（.view.on .pagewrap）自动跑，不用 JS 管。
  var TRANSITION_MS = 380;
  var leaveTimers = {};

  function show(screen, isBack, skipHistory) {
    if (!SCREENS[screen]) screen = "home";
    if (screen === current) return;
    if (!skipHistory) {
      if (isBack) { history.pop(); }
      else if (current) { history.push(current); }
    }
    var previous = current;
    current = screen;

    var dir = isBack ? "back" : "fwd";
    var old = previous ? $("v-" + previous) : null;
    var view = $("v-" + screen);

    // 出场：先摘掉 .on（这会把 opacity 拉到 0），再挂方向类把位移补上；
    // visibility 的翻转被 transition-delay 压到过渡结束之后，所以旧屏不会瞬间消失。
    if (old && old !== view) {
      old.classList.remove("on", "enter-fwd", "enter-back");
      old.classList.add("leave-" + dir);
      if (leaveTimers[previous]) clearTimeout(leaveTimers[previous]);
      leaveTimers[previous] = setTimeout(function () {
        old.classList.remove("leave-fwd", "leave-back");
      }, TRANSITION_MS);
    }

    if (view) {
      view.classList.remove("leave-fwd", "leave-back");
      view.classList.remove("enter-fwd", "enter-back");
      view.classList.add("enter-" + dir);       // 起点：偏移 + 透明
      void view.offsetWidth;                    // 强制重排，保证从起点开始过渡
      requestAnimationFrame(function () {
        view.classList.add("on");               // 终点：归位 + 不透明
        view.classList.remove("enter-" + dir);
      });
    }

    setRail(screen);
    $("ghostNum").textContent = (RAIL_OF[screen] !== undefined ? ("0" + (RAIL_OF[screen] + 1)) : "01");

    if (!isBack) playSlices();
    if (screen === "home") updateHome();
    if (screen === "chat") { renderChat(); scrollLog(); }
    if (screen === "settings" || screen === "api") render();
    post({ type: "screen", screen: screen });
  }

  // 前进：6 片半透明切片自上落下；返回：不加这层（一步到位，只留位移淡入）
  var sliceTimer = null;
  function playSlices() {
    var box = $("slices");
    if (!box) return;
    box.classList.remove("on");
    void box.offsetWidth;              // 强制重排，动画才会重播
    box.classList.add("on");
    if (sliceTimer) clearTimeout(sliceTimer);
    sliceTimer = setTimeout(function () { box.classList.remove("on"); }, 320);
  }

  function goBack() {
    var prev = history.length ? history[history.length - 1] : "home";
    show(prev, true);
  }

  // ============================================================
  // 字段渲染：全部走设计稿的组件
  //   分区 → section.sec > header.shead(.sno/.sen)
  //   容器 → .panelbox > .rows
  //   行   → .row > .lb(标签+英文小字) + 控件 + .val + .note
  // ============================================================
  var RANGE_MIN = {}, RANGE_MAX = {}, RANGE_STEP = {};

  function isDependencySource(key) {
    for (var s = 0; s < sections.length; s++) {
      var fs = sections[s].fields || [];
      for (var i = 0; i < fs.length; i++) {
        var req = fs[i].requires;
        if (req && req.key === key) return true;
      }
    }
    return false;
  }

  function visible(f) {
    if (!f.requires) return true;
    return String(values[f.requires.key]) === String(f.requires.value);
  }

  // 数值行：设计稿的 .rangebtn + .val，两头都能改（拖滑块 / 直接敲数字）
  function rangeHtml(f, v) {
    var min = f.min === undefined ? 0 : Number(f.min);
    var max = f.max === undefined ? 1 : Number(f.max);
    var step = f.step === undefined ? 0.01 : Number(f.step);
    var num = Number(v);
    if (!isFinite(num)) num = min;
    var ratio = (max > min) ? Math.max(0, Math.min(1, (num - min) / (max - min))) : 0;
    return '<span class="rangebtn" role="slider" tabindex="0" data-key="' + esc(f.key) + '" data-role="range"' +
      ' data-min="' + min + '" data-max="' + max + '" data-step="' + step + '"' +
      ' aria-valuemin="' + min + '" aria-valuemax="' + max + '" aria-valuenow="' + num + '">' +
      '<span class="track"></span><span class="fill" style="width:' + (ratio * 100).toFixed(2) + '%"></span>' +
      '<span class="knob" style="left:' + (ratio * 100).toFixed(2) + '%"></span></span>' +
      '<span class="val' + (max >= 1000 ? ' wide' : '') + '">' +
      '<input type="number" data-key="' + esc(f.key) + '" data-role="num" value="' + esc(v) + '"' +
      ' step="' + step + '" aria-label="' + esc(f.label) + '"></span>';
  }

  function controlHtml(f) {
    var v = values[f.key] === undefined ? "" : String(values[f.key]);

    if (f.kind === "bool") {
      var on = (v === "true" || v === "1");
      // 设计稿的开关就是 .tog 的「开启/关闭」两格
      return '<span class="tog" data-tog role="radiogroup" aria-label="' + esc(f.label) + '">' +
        '<button type="button" data-key="' + esc(f.key) + '" data-role="enum" data-value="true"' +
        (on ? ' class="on" aria-checked="true"' : ' aria-checked="false"') + '>开启</button>' +
        '<button type="button" data-key="' + esc(f.key) + '" data-role="enum" data-value="false"' +
        (!on ? ' class="on" aria-checked="true"' : ' aria-checked="false"') + '>关闭</button></span>';
    }

    if (f.kind === "enum") {
      var opts = String(f.options || "").split("|").filter(Boolean);
      var html = '<span class="tog" data-tog role="radiogroup" aria-label="' + esc(f.label) + '">';
      for (var i = 0; i < opts.length; i++) {
        html += '<button type="button" data-key="' + esc(f.key) + '" data-role="enum" data-value="' + esc(opts[i]) + '"' +
          (opts[i] === v ? ' class="on" aria-checked="true"' : ' aria-checked="false"') +
          (f.locked ? ' disabled' : '') + '>' + esc(opts[i]) + '</button>';
      }
      html += '</span>';
      if (f.locked) {
        html += '<span class="lockhint">已锁定 · 要换人设请先删除 prompts/yandere.chr</span>';
      }
      return html;
    }

    if (f.kind === "preset") {
      var list = presets[f.key] || [];
      if (!list.length) return '<span class="tiny">—</span>';
      var target = values[f.presetTarget] === undefined ? "" : String(values[f.presetTarget]);
      // 服务商：短列表，用设计稿的 .dd 轮换（点一下换一个）
      if (list.length <= 6) {
        var current = null;
        for (var c = 0; c < list.length; c++) if (list[c].value === target) current = list[c];
        if (!current) current = { label: target || "未选择", value: target };
        return '<span class="dd" data-key="' + esc(f.key) + '" data-role="dd" title="点一下换一个">' +
          '<span class="v">' + esc(current.label) + '</span><span class="cx">▾</span></span>';
      }
      // 模型：可能几十个，逐格点更实际（设计稿的 .dd 轮换在这里不适用）
      var grid = '<span class="pset-grid">';
      for (var n = 0; n < list.length; n++) {
        grid += '<button type="button" class="pset" aria-pressed="' + (list[n].value === target ? "true" : "false") +
          '" data-key="' + esc(f.key) + '" data-role="preset" data-value="' + esc(list[n].value) +
          '" title="' + esc(list[n].value) + '">' + esc(list[n].label) + '</button>';
      }
      return grid + '</span>';
    }

    if (f.kind === "multi") {
      var all = presets[f.key] || [];
      if (!all.length) return '<span class="tiny">—</span>';
      var picked = {};
      String(v || "").split(",").forEach(function (x) { picked[x.trim()] = 1; });
      var ms = '<span class="ms">';
      for (var k = 0; k < all.length; k++) {
        ms += '<label><input type="checkbox" data-key="' + esc(f.key) + '" data-role="multi"' +
          (picked[all[k].value] ? " checked" : "") + ' data-value="' + esc(all[k].value) + '">' +
          esc(all[k].label) + '</label>';
      }
      return ms + '</span>';
    }

    if (f.kind === "double" || f.kind === "int") {
      return rangeHtml(f, v);
    }

    if (f.kind === "longtext") {
      return '<span class="ctl grow" style="height:auto;padding:var(--s2) var(--s3)">' +
        '<textarea rows="3" data-key="' + esc(f.key) + '" data-role="text" style="width:100%;background:none;' +
        'border:0;outline:0;color:var(--tx-hi);font-family:var(--ff-jp);font-size:var(--fs-ui);' +
        'resize:vertical;line-height:1.6">' + esc(v) + '</textarea></span>';
    }

    // text / secret：设计稿的 .ctl + .sx（密钥的显隐就挂在 .sx 上）
    var type = (f.secret && !revealed[f.key]) ? "password" : "text";
    var out = '<span class="ctl grow"><input type="' + type + '" data-key="' + esc(f.key) + '" data-role="text"' +
      ' value="' + esc(v) + '" spellcheck="false" placeholder="' + esc(f.note || "") + '">';
    if (f.secret) {
      out += '<span class="sx" data-role="reveal" data-key="' + esc(f.key) + '" style="cursor:pointer">' +
        (revealed[f.key] ? "HIDE" : "SHOW") + '</span>';
    }
    return out + '</span>';
  }

  function rowHtml(f) {
    var err = errors[f.key];
    var note = err ? '<span class="note err" title="' + esc(err) + '">' + esc(err) + '</span>'
      : '<span class="note">' + esc(f.note || "") + '</span>';
    // 预设行的「值」列显示当前取值，其余行让 .val 交给控件自己（数值行）
    var val = (f.kind === "preset" || f.kind === "multi") ? '<span class="val"></span>' : '';
    return '<div class="row' + (visible(f) ? "" : " hide") + (f.locked ? " lockedrow" : "") + '" data-row="' + esc(f.key) + '"' +
      (visible(f) ? "" : ' style="display:none"') + '>' +
      '<span class="lb">' + esc(f.label) +
      (f.labelEn ? '<em>' + esc(f.labelEn) + '</em>' : '') + '</span>' +
      controlHtml(f) + val + note + '</div>';
  }

  function sectionHtml(section) {
    var rows = "";
    var fields = section.fields || [];
    for (var i = 0; i < fields.length; i++) rows += rowHtml(fields[i]);
    return '<section class="sec"><header class="shead"><span class="sno">' + esc(section.no || "") + '</span>' +
      '<h3>' + esc(section.title) + '</h3>' +
      (section.titleEn ? '<span class="sen">' + esc(section.titleEn) + '</span>' : '') + '</header>' +
      '<div class="panelbox" style="padding:var(--s5)"><div class="rows">' + rows + '</div></div></section>';
  }

  // 数字格式化：滑块上显示的 .val 保持和后端一样的精度
  function formatValue(f, num) {
    var step = f.step === undefined ? 0.01 : Number(f.step);
    if (f.kind === "int" || step >= 1) return String(Math.round(num));
    var decimals = step >= 0.1 ? 1 : 2;
    return num.toFixed(decimals);
  }

  // API 屏只放「接口与鉴权」那一节，其余全进设置屏
  function splitSections() {
    var api = [], rest = [];
    for (var i = 0; i < sections.length; i++) {
      var t = (sections[i].title || "") + (sections[i].titleEn || "") + (sections[i].key || "");
      if (/api|接口|鉴权|服务商|模型/i.test(t) && api.length === 0) api.push(sections[i]);
      else rest.push(sections[i]);
    }
    if (!api.length && sections.length) { api.push(sections[0]); rest = sections.slice(1); }
    return { api: api, rest: rest };
  }

  function render() {
    var parts = splitSections();
    var apiHtml = "", setHtml = "";
    for (var i = 0; i < parts.api.length; i++) apiHtml += sectionHtml(parts.api[i], i);
    for (var j = 0; j < parts.rest.length; j++) setHtml += sectionHtml(parts.rest[j], j);
    if ($("apiCards")) {
      $("apiCards").innerHTML = apiHtml || '<section class="sec"><div class="panelbox" style="padding:var(--s5)">' +
        '<div class="rows"><div class="row"><span class="lb">暂无字段</span></div></div></div></section>';
    }
    if ($("setCards")) {
      $("setCards").innerHTML = setHtml || '<section class="sec"><div class="panelbox" style="padding:var(--s5)">' +
        '<div class="rows"><div class="row"><span class="lb">暂无字段</span></div></div></div></section>';
    }
    syncDependents();
    markDirty();
    renderProbe();
  }

  function syncDependents() {
    var rows = document.querySelectorAll(".srow[data-row]");
    for (var i = 0; i < rows.length; i++) {
      var key = rows[i].getAttribute("data-row");
      var f = findField(key);
      if (!f || !f.requires) continue;
      if (visible(f)) rows[i].classList.remove("hide"); else rows[i].classList.add("hide");
    }
  }

  function findField(key) {
    for (var s = 0; s < sections.length; s++) {
      var fs = sections[s].fields || [];
      for (var i = 0; i < fs.length; i++) if (fs[i].key === key) return fs[i];
    }
    return null;
  }

  function changedKeys() {
    var out = [];
    for (var k in values) {
      if (!Object.prototype.hasOwnProperty.call(values, k)) continue;
      if (String(values[k]) !== String(baseline[k] === undefined ? "" : baseline[k])) out.push(k);
    }
    return out;
  }

  function markDirty() {
    var n = changedKeys().length;
    var text = n ? ("未保存 · " + n + " 项改动") : "未修改";
    var cls = n ? "warn" : "on";
    ["setDot", "setDot2"].forEach(function (id) { if ($(id)) $(id).className = "dot " + cls; });
    ["setState", "setState2"].forEach(function (id) { if ($(id)) $(id).textContent = text; });
  }

  // ---- 事件（整页委托：原型里不存在死键，这里也不留） ----
  document.addEventListener("click", function (ev) {
    var t = ev.target;
    // 点弹层外面的暗底 = 取消（弹层本身是 .overlay，箱体是 .ovbox）
    if (t.classList && t.classList.contains("overlay")) {
      var open0 = document.querySelectorAll(".overlay.on");
      for (var n = 0; n < open0.length; n++) open0[n].classList.remove("on");
      return;
    }
    if (t.closest && t.closest("[data-drag]")) {
      // 窗口没有标题栏：交给宿主进系统移动循环
      post({ type: "drag" });
      return;
    }
    var hit = t.closest ? t.closest("[data-go],[data-go-view],[data-back],[data-overlay],[data-overlay-close],[data-act],[data-role]") : null;
    if (!hit) return;

    if (hit.hasAttribute("data-overlay")) {
      var ov = $("ov" + hit.getAttribute("data-overlay").replace(/^./, function (c) { return c.toUpperCase(); }));
      if (ov) ov.classList.add("on");
      return;
    }
    if (hit.hasAttribute("data-overlay-close")) {
      var open = document.querySelectorAll(".overlay.on");
      for (var i = 0; i < open.length; i++) open[i].classList.remove("on");
      return;
    }
    if (hit.hasAttribute("data-back")) { goBack(); return; }
    if (hit.hasAttribute("data-go")) {
      var target = hit.getAttribute("data-go");
      if (hit.closest(".rail")) { history = []; show(target, false, true); }
      else show(target);
      return;
    }
    if (hit.hasAttribute("data-go-view")) {
      var view = hit.getAttribute("data-go-view");
      if (hit.getAttribute("data-chat-action") === "resume") post({ type: "chatResume" });
      else post({ type: "chatNew" });
      show(view);
      return;
    }

    var delId = hit.getAttribute("data-del");
    if (delId) {
      post({ type: "chatDelete", id: parseInt(delId, 10) });
      return;
    }
    if (hit.getAttribute("data-act") === "chatReset") {
      // 两段式确认：第一次点变成「再点一次确认」，4 秒内不再点就自动取消
      if (hit.getAttribute("data-armed") === "1") {
        hit.removeAttribute("data-armed");
        hit.textContent = "重置记忆库";
        post({ type: "chatReset" });
      } else {
        hit.setAttribute("data-armed", "1");
        hit.textContent = "再点一次确认重置";
        setTimeout(function () {
          if (hit.getAttribute("data-armed") === "1") {
            hit.removeAttribute("data-armed");
            hit.textContent = "重置记忆库";
          }
        }, 4000);
      }
      return;
    }

    var role = hit.getAttribute("data-role");
    var key = hit.getAttribute("data-key");
    if (role === "bool") {
      var on = hit.getAttribute("aria-checked") === "true";
      hit.setAttribute("aria-checked", on ? "false" : "true");
      setValue(key, on ? "false" : "true");
      return;
    }
    if (role === "enum") {
      if (hit.disabled) return;
      setValue(key, hit.getAttribute("data-value"));
      var group = hit.parentNode;
      var btns = group.querySelectorAll("button");
      for (var b = 0; b < btns.length; b++) {
        if (btns[b] === hit) { btns[b].classList.add("on"); btns[b].setAttribute("aria-checked", "true"); }
        else { btns[b].classList.remove("on"); btns[b].setAttribute("aria-checked", "false"); }
      }
      return;
    }
    if (role === "reveal") { revealed[key] = !revealed[key]; render(); return; }
    if (role === "dd") { cyclePreset(key); return; }
    if (role === "preset") {
      var f = findField(key);
      if (f && f.presetTarget) setValue(f.presetTarget, hit.getAttribute("data-value"));
      // 旧的取值通道：宿主收到 presetsChanged 会顺手收敛「服务商 + 模型」这一对，
      // 并把新的可用模型回推（presetOptions）。必须带上 values，否则纠正只停在宿主那边。
      post({ type: "presetsChanged", key: key, value: hit.getAttribute("data-value"), values: values });
      return;
    }

    var act = hit.getAttribute("data-act");
    if (act === "save") {
      post({ type: "save", values: values });
      setStatus("正在写入 app.db…");
    }
    else if (act === "reload") { post({ type: "reload" }); }
    else if (act === "restore") { post({ type: "restore" }); }
    else if (act === "test") { post({ type: "test", values: values }); setConn("warn", "测试中…"); }
    else if (act === "probe") {
      post({ type: "probe", values: values });
      probeState = { kind: 1, text: "测试中…", models: probeState.models, source: probeState.source };
      renderProbe();
    }
    else if (act === "makedefault") { post({ type: "makedefault", values: values }); }
    else if (act === "quit") { post({ type: "quit" }); }
    else if (act === "close") { post({ type: "close" }); }
    else if (act === "refreshPet") {
      // 桌宠刷新：宿主会重读配置 + 重载模型，然后把状态回推过来
      post({ type: "refreshPet" });
      if ($("homeBm")) $("homeBm").textContent = "REFRESHING…";
    }
    else if (act === "send") { sendChat(); }
  });

  // 复选框 / 滑块 / 文本：用 input 事件，边打字边入草稿
  document.addEventListener("input", function (ev) {
    var t = ev.target;
    if (!t || !t.getAttribute) return;
    var role = t.getAttribute("data-role");
    var key = t.getAttribute("data-key");
    if (!key) return;
    if (role === "multi") {
      var picked = [];
      var boxes = document.querySelectorAll('input[data-role="multi"][data-key="' + key + '"]');
      for (var i = 0; i < boxes.length; i++) if (boxes[i].checked) picked.push(boxes[i].getAttribute("data-value"));
      setValue(key, picked.join(","));
      return;
    }
    if (role === "range" || role === "text" || role === "num") {
      setValue(key, t.value);
      var rv = document.querySelector('[data-rv="' + key.replace(/"/g, '\\"') + '"]');
      if (rv && role === "range") rv.textContent = t.value;
      // 同一个字段的滑块和数字框要保持同步
      var peers = document.querySelectorAll('[data-key="' + key + '"][data-role="range"],[data-key="' + key + '"][data-role="num"],[data-key="' + key + '"][data-role="text"]');
      for (var p = 0; p < peers.length; p++) {
        if (peers[p] === t || peers[p].tagName !== "INPUT") continue;
        peers[p].value = t.value;
      }
      // 手敲数字时把滑块也推到位
      if (role === "num") {
        var btn = document.querySelector('.rangebtn[data-key="' + key + '"]');
        if (btn) {
          var min = Number(btn.getAttribute("data-min"));
          var max = Number(btn.getAttribute("data-max"));
          var num = Number(t.value);
          if (isFinite(num)) {
            btn.setAttribute("aria-valuenow", String(Math.max(min, Math.min(max, num))));
            paintRange(btn);
          }
        }
      }
    }
  });

  function setValue(key, value) {
    values[key] = value;
    if (isDependencySource(key)) syncDependents();
    markDirty();
  }

  function setStatus(status) {
    if (!$("setStatus")) return;
    // 宿主有两种下发方式：直接给字符串，或给 { text, kind }（state 消息就是后者）。
    // 不想清楚这一层，页面上就会出现 [object Object]。
    var text = status;
    if (status && typeof status === "object") text = status.text;
    text = (text === undefined || text === null) ? "" : String(text);
    $("setStatus").textContent = text;
    // 主菜单底栏也跟着显示一次，否则刷新桌宠这类动作在首页看不到回执
    if ($("homeBm") && text) $("homeBm").textContent = text;
  }
  function setConn(kind, text) {
    if ($("connDot")) $("connDot").className = "dot " + (kind || "");
    if ($("connState")) $("connState").textContent = text || "";
  }

  // ============================================================
  // 服务商探测：模型清单以服务商 /models 的返回为准，内置表只做兜底
  // ============================================================
  var probeState = { kind: 0, text: "", models: 0, source: "builtin" };

  function renderProbe() {
    var hit = "none";
    if (probeState.kind === 1) hit = "busy";
    else if (probeState.kind === 2) hit = "ok";
    else if (probeState.kind === 3) hit = "bad";

    var lamps = document.querySelectorAll("#apiLamps .lamp");
    for (var i = 0; i < lamps.length; i++) {
      lamps[i].classList.toggle("hit", lamps[i].getAttribute("data-state") === hit);
    }
    if ($("apiLine")) {
      $("apiLine").textContent = probeState.text || "READY · 等待测试";
    }
    if ($("apiSrc")) {
      $("apiSrc").textContent = probeState.source === "provider"
        ? ("模型 · 服务商提供 " + probeState.models + " 个")
        : (probeState.models > 0 ? "模型 · 内置兜底" : "模型 · 未知（先测一次）");
    }
    // 顶栏那颗灯也跟着走：一眼能看出链路状态
    var kind = probeState.kind === 2 ? "on" : (probeState.kind === 3 ? "err" : (probeState.kind === 1 ? "warn" : ""));
    ["homeDot", "apiDot"].forEach(function (id) {
      if ($(id)) $(id).className = "dot " + kind;
    });
    if ($("homeApi")) {
      $("homeApi").textContent = probeState.kind === 2 ? "API 可达"
        : (probeState.kind === 3 ? "API 不可达" : (probeState.kind === 1 ? "测试中…" : "API 未测试"));
    }
    if ($("apiState")) {
      $("apiState").textContent = probeState.kind === 2 ? "可达"
        : (probeState.kind === 3 ? "失败" : (probeState.kind === 1 ? "测试中" : "未测试"));
    }
  }

  // ============================================================
  // 滑块：按下/拖动都在 .rangebtn 上算比例（设计稿的 .fill/.knob 只是显示层）
  // ============================================================
  var dragging = null;

  function valueFromPointer(btn, clientX) {
    var rect = btn.getBoundingClientRect();
    var min = Number(btn.getAttribute("data-min"));
    var max = Number(btn.getAttribute("data-max"));
    var step = Number(btn.getAttribute("data-step")) || 0.01;
    var ratio = rect.width > 0 ? (clientX - rect.left) / rect.width : 0;
    ratio = Math.max(0, Math.min(1, ratio));
    var raw = min + (max - min) * ratio;
    var snapped = Math.round(raw / step) * step;
    // 浮点步长会带出 0.30000000000000004 这种尾巴，按步长的小数位收一下
    var decimals = (String(step).split(".")[1] || "").length;
    return Number(snapped.toFixed(Math.min(6, decimals)));
  }

  function paintRange(btn) {
    var min = Number(btn.getAttribute("data-min"));
    var max = Number(btn.getAttribute("data-max"));
    var num = Number(btn.getAttribute("aria-valuenow"));
    var ratio = (max > min) ? Math.max(0, Math.min(1, (num - min) / (max - min))) : 0;
    var fill = btn.querySelector(".fill");
    var knob = btn.querySelector(".knob");
    if (fill) fill.style.width = (ratio * 100).toFixed(2) + "%";
    if (knob) knob.style.left = (ratio * 100).toFixed(2) + "%";
  }

  // 数值闪一下：拖动滑块时让人看见是哪一行在变（动画类用完就摘掉，下次才重放）
  function flashValue(key) {
    var row = document.querySelector('.row[data-row=' + JSON.stringify(key) + '] .val');
    if (!row) return;
    row.classList.remove('flash');
    void row.offsetWidth;
    row.classList.add('flash');
    setTimeout(function () { row.classList.remove('flash'); }, 420);
  }

  function applyRange(btn, num) {
    var key = btn.getAttribute("data-key");
    var f = findField(key);
    btn.setAttribute("aria-valuenow", String(num));
    paintRange(btn);
    var shown = f ? formatValue(f, num) : String(num);
    flashValue(key);
    var row = btn.closest(".row");
    var input = row ? row.querySelector('.val input[data-role="num"]') : null;
    if (input && String(input.value) !== String(num)) input.value = shown;
    setValue(key, String(num));
  }

  document.addEventListener("pointerdown", function (ev) {
    var btn = ev.target.closest ? ev.target.closest(".rangebtn") : null;
    if (!btn) return;
    ev.preventDefault();
    dragging = btn;
    applyRange(btn, valueFromPointer(btn, ev.clientX));
    if (btn.setPointerCapture) { try { btn.setPointerCapture(ev.pointerId); } catch (e) { /* 忽略 */ } }
  });
  document.addEventListener("pointermove", function (ev) {
    if (!dragging) return;
    applyRange(dragging, valueFromPointer(dragging, ev.clientX));
  });
  document.addEventListener("pointerup", function () { dragging = null; });
  document.addEventListener("pointercancel", function () { dragging = null; });

  // 键盘也能调：聚焦滑块后用 ←/→
  document.addEventListener("keydown", function (ev) {
    var btn = ev.target.closest ? ev.target.closest(".rangebtn") : null;
    if (!btn) return;
    var step = Number(btn.getAttribute("data-step")) || 0.01;
    var min = Number(btn.getAttribute("data-min"));
    var max = Number(btn.getAttribute("data-max"));
    var cur = Number(btn.getAttribute("aria-valuenow"));
    if (ev.key === "ArrowRight" || ev.key === "ArrowUp") {
      ev.preventDefault();
      applyRange(btn, Math.min(max, cur + step));
    } else if (ev.key === "ArrowLeft" || ev.key === "ArrowDown") {
      ev.preventDefault();
      applyRange(btn, Math.max(min, cur - step));
    }
  });

  // .dd：点一下换下一个（设计稿的「点一下换一个供应商」）
  function cyclePreset(key) {
    var f = findField(key);
    if (!f) return;
    var list = presets[key] || [];
    if (!list.length) return;
    var target = f.presetTarget;
    var current = values[target] === undefined ? "" : String(values[target]);
    var index = -1;
    for (var i = 0; i < list.length; i++) if (list[i].value === current) index = i;
    var next = list[(index + 1) % list.length];
    setValue(target, next.value);
    var node = document.querySelector('.dd[data-key="' + key + '"] .v');
    if (node) node.textContent = next.label;
    post({ type: "presetsChanged", key: key, value: next.value, values: values });
  }

  // ============================================================
  // 主菜单摘要
  // ============================================================
  function updateHome() {
    var model = values["api.model_default"] || "—";
    if ($("homeModel")) $("homeModel").textContent = model;
    if ($("chatBm")) $("chatBm").textContent = "MODEL " + model;
    if ($("petModel")) $("petModel").textContent = values["model.default_id"] || "—";
    if ($("petPersona")) $("petPersona").textContent = personaText();
    if ($("homePersona")) $("homePersona").textContent = personaText();
    var rounds = values["context.max_rounds"] || "—";
    var budget = values["context.max_tokens"] || "";
    var ctx = rounds + " 轮" + (budget ? " · " + budget + " tok" : "");
    if ($("homeCtx")) $("homeCtx").textContent = ctx;
    if ($("petCtx")) $("petCtx").textContent = ctx;
    var tools = values["mcp.enabled_tools"];
    var toolText = tools ? String(tools).split(",").filter(Boolean).length + " 项" : "全部";
    if ($("homeTools")) $("homeTools").textContent = toolText;
    if ($("petTools")) $("petTools").textContent = toolText;
    if ($("chatTools")) $("chatTools").textContent = values["mcp.enabled_tools"] || "全部工具";
    var quick = values["quick.enabled"] === "true";
    if ($("homeQuick")) $("homeQuick").textContent = quick ? (values["quick.mode"] || "开") : "关闭";
  }

  function personaText() {
    var mode = values["quick.mode"] || "关闭";
    if (values["quick.enabled"] !== "true") return "关闭";
    return mode;
  }

  function setApiState(ok, text) {
    var dot = $("homeDot"), lab = $("homeApi"), adot = $("apiDot"), alab = $("apiState");
    if (dot) dot.className = "dot " + (ok ? "on" : "err");
    if (lab) lab.textContent = text;
    if (adot) adot.className = "dot " + (ok ? "on" : "err");
    if (alab) alab.textContent = text;
  }

  // ============================================================
  // 对话
  // ============================================================
  function bubbleHtml(m) {
    var cls = m.role === "me" ? " me" : (m.role === "tool" ? " tool" : (m.role === "sys" ? " sys" : ""));
    var who = m.who ? '<span class="who">' + esc(m.who) + '</span>' : "";
    var body = m.role === "tool" && m.tag
      ? '<em>' + esc(m.tag) + '</em>' + esc(m.text)
      : esc(m.text);
    // 每条气泡右上角的删除按钮：data-del 带消息 id，由点击委托处理
    var del = m.id ? '<button type="button" class="del" data-del="' + m.id + '" title="删除这条对话">×</button>' : "";
    return '<div class="bub' + cls + '">' + who + body + del + '</div>';
  }

  var animatedBubbles = 0;   // 已经播过入场动画的条数：流式增量重渲染时不重播

  function renderChat() {
    var log = $("chatLog");
    if (!log) return;
    if (!chatMessages.length) {
      log.innerHTML = '<div class="bub sys">还没有会话记录。在下面输入一句话开始。</div>';
      animatedBubbles = 0;
    } else {
      var html = "";
      for (var i = 0; i < chatMessages.length; i++) {
        // 只给「比上次多出来的」那几条挂 .in：否则每次流式增量重渲染，
        // 整屏气泡都会重新播一遍入场动画，看起来就是一直在抖。
        var fresh = (i >= animatedBubbles) ? " in" : "";
        html += bubbleHtml(chatMessages[i]).replace('class="bub', 'class="bub' + fresh);
      }
      log.innerHTML = html;
      animatedBubbles = chatMessages.length;
    }
    var last = log.lastElementChild;
    if (last && chatMessages.length && chatMessages[chatMessages.length - 1].streaming) last.classList.add("stream");
    updateHome();
  }

  function scrollLog() {
    var log = $("chatLog");
    if (log) log.scrollTop = log.scrollHeight;
  }

  function sendChat() {
    var inp = $("chatInp");
    if (!inp) return;
    var text = String(inp.value || "").trim();
    if (!text) return;
    inp.value = "";
    chatMessages.push({ role: "me", who: "You", text: text });
    renderChat(); scrollLog();
    post({ type: "chatSend", text: text });
    setChatState("warn", "等待回复…");
  }

  function setChatState(kind, text) {
    if ($("chatDot")) $("chatDot").className = "dot " + kind;
    if ($("chatState")) $("chatState").textContent = text;
  }

  $("chatInp") && $("chatInp").addEventListener("keydown", function (ev) {
    if (ev.key === "Enter" && (ev.ctrlKey || ev.metaKey)) { ev.preventDefault(); sendChat(); }
    if (ev.key === "Escape") { ev.preventDefault(); goBack(); }
  });

  // ============================================================
  // 加载屏
  // ============================================================
  var loadSteps = [
    { key: "config", label: "读取配置", en: "Config", note: "app.db" },
    { key: "ui", label: "构建界面", en: "UI", note: "WebView2" },
    { key: "render", label: "初始化渲染", en: "Renderer", note: "D3D11 / DirectComposition" },
    { key: "model", label: "载入 Live2D 模型", en: "Model", note: "着色器编译约 50 秒" }
  ];
  var loadDone = {};

  function renderLoadSteps() {
    var host = $("loadSteps");
    if (!host) return;
    var html = "";
    for (var i = 0; i < loadSteps.length; i++) {
      var st = loadSteps[i];
      var done = !!loadDone[st.key];
      html += '<div class="row"><span class="lb">' + esc(st.label) + '<em>' + esc(st.en) + '</em></span>' +
        '<span class="lamp ' + (done ? "ok hit" : "none hit") + '" style="flex:none;padding:0"><i></i>' +
        (done ? "完成" : "等待") + '</span>' +
        '<span class="val"></span><span class="note">' + esc(st.note) + '</span></div>';
    }
    host.innerHTML = html;
  }

  function setProgress(percent, label) {
    var p = Math.max(0, Math.min(100, Number(percent) || 0));
    if ($("loadPct")) $("loadPct").textContent = Math.round(p) + "%";
    if ($("loadFill")) $("loadFill").style.insetInlineEnd = (100 - p) + "%";
    if (label && $("loadStep")) $("loadStep").value = label;
    // 百分比映射到四个阶段，做到哪一步就点亮哪一步
    loadDone.config = p >= 10;
    loadDone.ui = p >= 30;
    loadDone.render = p >= 55;
    loadDone.model = p >= 100;
    ["loadDot", "loadDot2"].forEach(function (id) {
      if ($(id)) $(id).className = "dot " + (p >= 100 ? "on" : "warn");
    });
    var phase = p >= 100 ? "完成" : (p >= 55 ? "渲染" : (p >= 30 ? "界面" : "配置"));
    ["loadPhase", "loadPhase2"].forEach(function (id) { if ($(id)) $(id).textContent = phase; });
    renderLoadSteps();
  }

  // ============================================================
  // 主题（？？？）：只换令牌，不动结构
  // ============================================================
  function applyTheme(t) {
    document.body.classList.toggle("yandere", !!(t && t.yandere));
    if (t && t.yandere) {
      // 主菜单里的角色牌也跟着改成血色语气
      if ($("railBuild")) $("railBuild").textContent = "？？？ ACTIVE";
    } else if ($("railBuild")) {
      $("railBuild").textContent = "AGENT HUB";
    }
  }

  // ============================================================
  // 键盘：↑↓ 选菜单 / Enter 确认 / Esc 返回
  // ============================================================
  var menuIndex = 0;
  function menuOptions() {
    return document.querySelectorAll("#col .opt");
  }
  function paintMenu() {
    var opts = menuOptions();
    for (var i = 0; i < opts.length; i++) opts[i].classList.toggle("sel", i === menuIndex);
    if (opts[menuIndex] && $("dsline")) {
      $("dsline").innerHTML = esc(opts[menuIndex].getAttribute("data-ds") || "") +
        '　<b>↑↓</b> 移动 · <b>Enter</b> 确认 · <b>Esc</b> 返回';
    }
  }
  document.addEventListener("keydown", function (ev) {
    if (current !== "home") {
      if (ev.key === "Escape") { ev.preventDefault(); goBack(); }
      return;
    }
    var opts = menuOptions();
    if (!opts.length) return;
    if (ev.key === "ArrowDown") { ev.preventDefault(); menuIndex = (menuIndex + 1) % opts.length; paintMenu(); }
    else if (ev.key === "ArrowUp") { ev.preventDefault(); menuIndex = (menuIndex - 1 + opts.length) % opts.length; paintMenu(); }
    else if (ev.key === "Enter") { ev.preventDefault(); opts[menuIndex].click(); }
    else if (ev.key === "Escape") {
      var open = document.querySelectorAll(".overlay.on");
      if (open.length) { for (var i = 0; i < open.length; i++) open[i].classList.remove("on"); }
    }
  });

  // 悬停主菜单项时给出说明
  document.addEventListener("mouseover", function (ev) {
    var opt = ev.target.closest ? ev.target.closest("#col .opt") : null;
    if (!opt) return;
    var opts = menuOptions();
    for (var i = 0; i < opts.length; i++) if (opts[i] === opt) menuIndex = i;
    paintMenu();
  });

  // ============================================================
  // 宿主消息
  // ============================================================
  function applyState(msg) {
    if (msg.sections) sections = msg.sections;
    if (msg.values) values = msg.values;
    if (msg.baseline) baseline = msg.baseline;
    if (msg.presets) presets = msg.presets;
    if (msg.probe) probeState = msg.probe;
    errors = msg.errors || {};
    applyTheme(msg.theme);
    setStatus(msg.status);
    render();
    setApiState(!!(values["api.api_key"] && values["api.base_url"]),
      values["api.api_key"] ? "API 已接入" : "API 未接入");
    updateHome();
    renderProbe();
    if (msg.screen && SCREENS[msg.screen]) show(msg.screen, false, true);
  }

  if (bridge) {
    bridge.addEventListener("message", function (ev) {
      var msg = ev.data || {};
      switch (msg.type) {
        case "init":
        case "state":
          applyState(msg);
          show(msg.screen || current, false, true);
          break;
        case "presetOptions":
          presets[msg.key] = msg.options || [];
          if (msg.values) { values = msg.values; }
          render();
          break;
        case "validation":
          errors = msg.errors || {};
          if (msg.values) values = msg.values;
          setStatus(msg.status);
          render();
          break;
        case "status":
          setStatus(msg.status);
          if (msg.conn) setConn(msg.conn.kind, msg.conn.text);
          if (msg.api) setApiState(msg.api.ok, msg.api.text);
          break;
        case "progress":
          show("loading", false, true);
          setProgress(msg.percent, msg.label);
          if (msg.percent >= 100) setTimeout(function () { show(msg.next || "home", false, true); }, 320);
          break;
        case "probeState":
          probeState = {
            kind: msg.kind || 0,
            text: msg.text || "",
            models: msg.models || 0,
            source: msg.source || "builtin"
          };
          renderProbe();
          break;
        case "chat":
          chatMessages = msg.messages || [];
          renderChat(); scrollLog();
          if (msg.model && $("chatBm")) $("chatBm").textContent = "MODEL " + msg.model;
          setChatState("on", msg.state || "就绪");
          break;
        case "chatDelta":
          if (!chatMessages.length) chatMessages.push({ role: "ai", who: "Assistant", text: "" });
          chatMessages[chatMessages.length - 1].text += msg.text || "";
          chatMessages[chatMessages.length - 1].streaming = true;
          renderChat(); scrollLog();
          break;
        case "chatDone":
          if (chatMessages.length) chatMessages[chatMessages.length - 1].streaming = false;
          renderChat(); setChatState("on", "就绪");
          break;
        case "chatTool":
          chatMessages.push({ role: "tool", tag: msg.tag || "tool", text: msg.text || "" });
          renderChat(); scrollLog();
          break;
        default:
          break;
      }
    });
    post({ type: "ready" });
  } else {
    // 无宿主（含 ?mock=1）：给一份静态样本，用来单独校对排版
    mockState();
  }

  function mockState() {
    sections = [
      { no: "01", title: "AI · 接口与鉴权", titleEn: "Endpoint", sub: "", fields: [
        { key: "api.provider_preset", label: "服务商", labelEn: "PROVIDER", note: "点一下换一个", kind: "preset", presetTarget: "api.base_url" },
        { key: "api.base_url", label: "服务地址", labelEn: "BASE URL", note: "OpenAI 兼容 /v1", kind: "text" },
        { key: "api.api_key", label: "API 密钥", labelEn: "API KEY", note: "只写进 app.db", kind: "text", secret: true },
        { key: "api.model_preset", label: "可用模型", labelEn: "MODELS", note: "随服务商变化", kind: "preset", presetTarget: "api.model_default" },
        { key: "api.model_default", label: "默认模型", labelEn: "MODEL", note: "对话默认", kind: "text" },
        { key: "stream.timeout_seconds", label: "请求超时", labelEn: "TIMEOUT", note: "秒", kind: "int", min: 5, max: 300, step: 5 }
      ] },
      { no: "02", title: "采样与随机惩罚", titleEn: "Sampling", sub: "", fields: [
        { key: "sampling.temperature", label: "温度", labelEn: "TEMPERATURE", note: "0 – 2", kind: "double", min: 0, max: 2, step: 0.05 },
        { key: "sampling.top_p", label: "核采样", labelEn: "TOP P", note: "0 – 1", kind: "double", min: 0, max: 1, step: 0.01 },
        { key: "context.max_rounds", label: "记忆轮数", labelEn: "MAX ROUNDS", note: "轮", kind: "int", min: 1, max: 64, step: 1 }
      ] },
      { no: "03", title: "人设与人格", titleEn: "Persona", sub: "", fields: [
        { key: "quick.enabled", label: "快速模式", labelEn: "QUICK", note: "只显示圆点", kind: "bool" },
        { key: "quick.mode", label: "人设", labelEn: "PERSONA", note: "删除 prompts/yandere.chr 可解锁",
          kind: "enum", options: "关闭|？？？", locked: true, requires: { key: "quick.enabled", value: "true" } }
      ] },
      { no: "04", title: "MCP · 工具绑定", titleEn: "Tools", sub: "", fields: [
        { key: "mcp.enabled_tools", label: "启用工具", labelEn: "TOOLS", note: "多选", kind: "multi" },
        { key: "mcp.confirm", label: "调用前确认", labelEn: "CONFIRM", note: "写文件/执行命令前弹窗", kind: "bool" }
      ] }
    ];
    values = {
      "api.provider": "deepseek", "api.base_url": "https://api.deepseek.com/v1",
      "api.api_key": "sk-xxxxxxxxxxxxxxxx", "api.model_default": "deepseek-flash", "api.timeout": "60",
      "sampling.temperature": "0.70", "sampling.top_p": "0.90", "context.max_rounds": "8",
      "stream.timeout_seconds": "60", "api.model_default": "deepseek-flash", "mcp.confirm": "true",
      "quick.enabled": "true", "quick.mode": "？？？",
      "mcp.enabled_tools": "web_search,file_read", "mcp.confirm": "true",
      "model.default_id": "wuwuwu"
    };
    baseline = JSON.parse(JSON.stringify(values));
    presets = {
      "api.provider_preset": [
        { label: "DeepSeek", value: "https://api.deepseek.com/v1" },
        { label: "OpenAI", value: "https://api.openai.com/v1" },
        { label: "智谱 GLM", value: "https://open.bigmodel.cn/api/paas/v4" }
      ],
      "api.model_preset": [
        { label: "deepseek-flash", value: "deepseek-flash" },
        { label: "deepseek-v4-pro", value: "deepseek-v4-pro" },
        { label: "deepseek-reasoner", value: "deepseek-reasoner" }
      ],
      "mcp.enabled_tools": [
        { label: "web_search", value: "web_search" }, { label: "file_read", value: "file_read" },
        { label: "run_shell", value: "run_shell" }, { label: "get_current_time", value: "get_current_time" }
      ]
    };
    probeState = { kind: 2, text: "HTTP 200 · 318ms · 3 个模型", models: 3, source: "provider" };
    chatMessages = [
      { id: 101, role: "ai", who: "Assistant", text: "桌宠中枢已就绪。在「设置」里选好供应商并保存后即可开始会话。" },
      { id: 102, role: "tool", tag: "MCP", text: "组装 CreateMessage：system + 历史 6 条 · 采样 T0.70 P0.90 · 工具 x3 · 流式" },
      { id: 103, role: "me", who: "You", text: "帮我查一下今天的天气，然后写入备忘录。" },
      { id: 104, role: "tool", tag: "tool_calls", text: "get_weather(q=\"今天 天气\")" },
      { id: 105, role: "ai", who: "Assistant", text: "北京今天晴，24°C，湿度 38%。已把结果写入备忘录。删除按钮在气泡右上角，鼠标悬停时出现。" }
    ];
    applyTheme({ yandere: !/(\\?|&)normal=1/.test(location.search) });
    render();
    updateHome();
    renderChat();
    setApiState(true, "API 已接入");
    setProgress(68, "正在编译 Live2D 着色器…");
    // 预览用：?mock=1&overlay=exit 直接把退出弹层打开，便于截图核对两个按钮
    if (/overlay=exit/.test(location.search)) {
      var mockOverlay = $("ovExit");
      if (mockOverlay) mockOverlay.classList.add("on");
    }
    var want = (location.hash || "").replace("#", "");
    if (SCREENS[want]) show(want, false, true);
    paintMenu();
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
