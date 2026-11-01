// Internal Manager panel. No server, launcher or standalone page.
export async function mount(root,ctx){
 const shadow=root.attachShadow({mode:"open"});
 shadow.innerHTML="<style>"+":host {\n  color-scheme: dark;\n  --bg: #080b10;\n  --surface: #101620;\n  --surface-2: #151e2a;\n  --surface-3: #1c2836;\n  --line: #273749;\n  --text: #eef5fb;\n  --muted: #8da0b2;\n  --cyan: #46dbe8;\n  --cyan-strong: #09adc1;\n  --amber: #ffba52;\n  --red: #ff6470;\n  --green: #55d899;\n  --shadow: 0 18px 60px rgb(0 0 0 / 36%);\n  font-family: Inter, \"Segoe UI\", \"Microsoft YaHei UI\", sans-serif;\n}\n\n* {\n  box-sizing: border-box;\n}\n\nbody {\n  margin: 0;\n  min-width: 1040px;\n  min-height: 100vh;\n  color: var(--text);\n  background:\n    radial-gradient(circle at 85% 0%, rgb(24 86 105 / 20%), transparent 30rem),\n    linear-gradient(135deg, #07090d, #0b1018 55%, #080d14);\n}\n\nbutton,\ninput,\nselect {\n  font: inherit;\n}\n\nbutton,\n.button-link {\n  min-height: 38px;\n  border: 1px solid var(--line);\n  border-radius: 8px;\n  padding: 0 14px;\n  color: var(--text);\n  background: var(--surface-2);\n  cursor: pointer;\n  transition: border-color 120ms ease, background 120ms ease, transform 120ms ease;\n}\n\nbutton:hover,\n.button-link:hover {\n  border-color: #4d6a83;\n  background: var(--surface-3);\n}\n\nbutton:active {\n  transform: translateY(1px);\n}\n\nbutton:disabled {\n  opacity: .45;\n  cursor: not-allowed;\n}\n\nbutton.primary,\n.button-link.primary {\n  color: #031014;\n  border-color: var(--cyan);\n  background: var(--cyan);\n  font-weight: 700;\n}\n\nbutton.primary:hover {\n  background: #7ee9f1;\n}\n\nbutton.danger {\n  color: #ffdfe2;\n  border-color: rgb(255 100 112 / 55%);\n  background: rgb(255 100 112 / 10%);\n}\n\nbutton.success {\n  color: #dffff0;\n  border-color: rgb(85 216 153 / 55%);\n  background: rgb(85 216 153 / 10%);\n}\n\n.button-link {\n  display: inline-flex;\n  align-items: center;\n  justify-content: center;\n  text-decoration: none;\n}\n\n.topbar {\n  height: 86px;\n  display: flex;\n  align-items: center;\n  justify-content: space-between;\n  padding: 0 24px;\n  border-bottom: 1px solid var(--line);\n  background: rgb(8 11 16 / 92%);\n}\n\n.topbar h1,\n.editor-heading h2,\n.section-heading h3,\n.dialog-heading h2 {\n  margin: 0;\n}\n\n.topbar h1 {\n  font-size: 23px;\n  letter-spacing: .02em;\n}\n\n.eyebrow {\n  margin: 0 0 5px;\n  color: var(--cyan);\n  font-size: 10px;\n  font-weight: 800;\n  letter-spacing: .16em;\n}\n\n.topbar-status,\n.editor-badges,\n.footer-actions,\n.editor-actions {\n  display: flex;\n  align-items: center;\n  gap: 9px;\n}\n\n.status-pill,\n.badge {\n  display: inline-flex;\n  align-items: center;\n  min-height: 25px;\n  border: 1px solid var(--line);\n  border-radius: 999px;\n  padding: 0 9px;\n  color: var(--muted);\n  background: rgb(255 255 255 / 3%);\n  font-size: 12px;\n}\n\n.status-pill.dirty {\n  color: #ffe4b7;\n  border-color: rgb(255 186 82 / 60%);\n}\n\n.status-pill.saving {\n  color: #c8faff;\n  border-color: rgb(70 219 232 / 60%);\n}\n\n.status-pill.error {\n  color: #ffd9dd;\n  border-color: rgb(255 100 112 / 60%);\n}\n\n.badge.accent {\n  color: #bff9ff;\n  border-color: rgb(70 219 232 / 45%);\n  background: rgb(70 219 232 / 8%);\n}\n\n.muted {\n  color: var(--muted);\n}\n\n.mono {\n  font-family: \"Cascadia Mono\", Consolas, monospace;\n  font-size: 12px;\n}\n\n.workspace {\n  display: grid;\n  grid-template-columns: 330px minmax(710px, 1fr);\n  height: calc(100vh - 144px);\n}\n\n.sidebar {\n  display: flex;\n  flex-direction: column;\n  min-height: 0;\n  border-right: 1px solid var(--line);\n  background: rgb(11 16 24 / 86%);\n}\n\n.sidebar-actions {\n  display: grid;\n  grid-template-columns: 1fr 1fr;\n  gap: 8px;\n  padding: 14px;\n}\n\n.sidebar-actions .primary {\n  grid-column: 1 / -1;\n}\n\n.tabs {\n  display: grid;\n  grid-template-columns: 1fr 1fr;\n  margin: 0 14px 13px;\n  border: 1px solid var(--line);\n  border-radius: 9px;\n  padding: 3px;\n  background: #080c12;\n}\n\n.tab {\n  border: 0;\n  background: transparent;\n}\n\n.tab.active {\n  color: #031014;\n  background: var(--cyan);\n  font-weight: 700;\n}\n\n.search-box {\n  display: flex;\n  flex-direction: column;\n  gap: 6px;\n  margin: 0 14px 10px;\n  color: var(--muted);\n  font-size: 12px;\n}\n\n.search-box.large {\n  margin: 0 0 12px;\n}\n\ninput,\nselect {\n  width: 100%;\n  min-height: 39px;\n  border: 1px solid var(--line);\n  border-radius: 7px;\n  outline: none;\n  padding: 8px 10px;\n  color: var(--text);\n  background: #0a1018;\n}\n\ninput:focus,\nselect:focus {\n  border-color: var(--cyan-strong);\n  box-shadow: 0 0 0 3px rgb(70 219 232 / 10%);\n}\n\ninput:disabled,\nselect:disabled {\n  color: var(--muted);\n  background: #10151d;\n}\n\ninput[type=\"checkbox\"],\ninput[type=\"radio\"] {\n  width: 16px;\n  min-height: 16px;\n  accent-color: var(--cyan);\n}\n\n.filter-row {\n  display: flex;\n  gap: 13px;\n  padding: 0 14px 10px;\n  color: var(--muted);\n  font-size: 12px;\n}\n\n.filter-row label {\n  display: flex;\n  align-items: center;\n  gap: 5px;\n}\n\n.list-caption {\n  padding: 0 14px 8px;\n  color: var(--muted);\n  font-size: 12px;\n}\n\n.machine-list {\n  min-height: 0;\n  overflow: auto;\n  padding: 0 8px 20px;\n}\n\n.machine-card {\n  width: 100%;\n  min-height: 76px;\n  display: grid;\n  grid-template-columns: 48px 1fr auto;\n  gap: 10px;\n  align-items: center;\n  margin: 0 0 5px;\n  padding: 9px;\n  text-align: left;\n  background: transparent;\n}\n\n.machine-card.active {\n  border-color: var(--cyan);\n  background: rgb(70 219 232 / 9%);\n}\n\n.machine-card.retired {\n  opacity: .58;\n}\n\n.machine-position {\n  display: flex;\n  flex-direction: column;\n  align-items: center;\n  justify-content: center;\n  min-height: 48px;\n  border: 1px solid var(--line);\n  border-radius: 7px;\n  background: #090e15;\n  color: var(--cyan);\n  font-weight: 800;\n}\n\n.machine-position small {\n  color: var(--muted);\n  font-size: 9px;\n  font-weight: 500;\n}\n\n.machine-copy {\n  min-width: 0;\n}\n\n.machine-copy strong,\n.machine-copy small {\n  display: block;\n  overflow: hidden;\n  text-overflow: ellipsis;\n  white-space: nowrap;\n}\n\n.machine-copy strong {\n  margin-bottom: 4px;\n  font-size: 13px;\n}\n\n.machine-copy small {\n  color: var(--muted);\n  font-size: 10px;\n}\n\n.machine-price {\n  color: #d3e2ed;\n  font-family: \"Cascadia Mono\", monospace;\n  font-size: 11px;\n}\n\n.main-panel {\n  min-width: 0;\n  overflow: auto;\n  padding: 20px 24px 60px;\n}\n\n.empty-state {\n  height: 100%;\n  display: flex;\n  flex-direction: column;\n  align-items: center;\n  justify-content: center;\n  color: var(--muted);\n  text-align: center;\n}\n\n.empty-state h2 {\n  margin: 8px 0;\n  color: var(--text);\n}\n\n.empty-state p {\n  width: 460px;\n  line-height: 1.7;\n}\n\n.empty-mark {\n  color: var(--cyan);\n  font-size: 56px;\n  filter: drop-shadow(0 0 18px rgb(70 219 232 / 35%));\n}\n\n.hidden {\n  display: none !important;\n}\n\n.editor {\n  max-width: 1180px;\n  margin: 0 auto;\n}\n\n.editor-heading,\n.section-heading,\n.dialog-heading {\n  display: flex;\n  align-items: flex-start;\n  justify-content: space-between;\n  gap: 18px;\n}\n\n.editor-heading {\n  margin-bottom: 16px;\n}\n\n.editor-heading h2 {\n  margin-top: 9px;\n}\n\n.editor-heading p {\n  margin: 5px 0 0;\n}\n\n.panel {\n  margin-bottom: 16px;\n  border: 1px solid var(--line);\n  border-radius: 12px;\n  padding: 18px;\n  background: linear-gradient(145deg, rgb(18 26 37 / 94%), rgb(12 18 27 / 94%));\n  box-shadow: 0 8px 28px rgb(0 0 0 / 15%);\n}\n\n.form-grid {\n  display: grid;\n  grid-template-columns: repeat(4, minmax(0, 1fr));\n  gap: 13px;\n}\n\n.form-grid label,\n.form-stack label,\n.read-only-field {\n  display: flex;\n  flex-direction: column;\n  gap: 6px;\n  color: #aebdca;\n  font-size: 12px;\n}\n\n.read-only-field strong {\n  min-height: 39px;\n  display: flex;\n  align-items: center;\n  border: 1px solid var(--line);\n  border-radius: 7px;\n  padding: 0 11px;\n  color: #d9e7f2;\n  background: rgb(255 255 255 / 2%);\n  font-weight: 600;\n}\n\n.form-grid .span-2 {\n  grid-column: span 2;\n}\n\n.form-grid small {\n  color: var(--muted);\n  line-height: 1.4;\n}\n\n.check-field {\n  flex-direction: row !important;\n  align-items: center;\n  align-self: end;\n  min-height: 39px;\n}\n\n.section-heading {\n  align-items: center;\n  margin-bottom: 10px;\n}\n\n.section-heading h3 {\n  font-size: 17px;\n}\n\n.section-note {\n  margin: 0 0 14px;\n  color: var(--muted);\n  font-size: 12px;\n  line-height: 1.6;\n}\n\n.weight-total {\n  border: 1px solid var(--line);\n  border-radius: 8px;\n  padding: 8px 12px;\n  color: var(--amber);\n  font-family: \"Cascadia Mono\", monospace;\n  font-size: 12px;\n}\n\n.weight-total.valid {\n  color: var(--green);\n  border-color: rgb(85 216 153 / 45%);\n}\n\n.entry-table {\n  display: flex;\n  flex-direction: column;\n  gap: 7px;\n}\n\n.entry-row {\n  display: grid;\n  grid-template-columns: 38px 54px minmax(190px, 1.6fr) minmax(150px, .8fr) minmax(130px, .7fr) 38px;\n  gap: 8px;\n  align-items: center;\n  border: 1px solid var(--line);\n  border-radius: 9px;\n  padding: 8px;\n  background: rgb(5 10 16 / 46%);\n}\n\n.entry-row.custom-row {\n  grid-template-columns: 38px 54px minmax(180px, 1.4fr) repeat(4, minmax(82px, .7fr)) 78px 38px;\n}\n\n.entry-index {\n  color: var(--cyan);\n  font-family: \"Cascadia Mono\", monospace;\n  font-weight: 800;\n  text-align: center;\n}\n\n.unit-thumb,\n.unit-placeholder {\n  width: 52px;\n  height: 52px;\n  border: 1px solid var(--line);\n  border-radius: 7px;\n  object-fit: contain;\n  background:\n    linear-gradient(135deg, rgb(70 219 232 / 9%), transparent),\n    #080c12;\n}\n\n.unit-placeholder {\n  display: grid;\n  place-items: center;\n  color: #4f6477;\n  font-size: 20px;\n}\n\n.unit-select-button {\n  min-width: 0;\n  height: 52px;\n  display: flex;\n  flex-direction: column;\n  align-items: flex-start;\n  justify-content: center;\n  text-align: left;\n}\n\n.unit-select-button strong,\n.unit-select-button small {\n  width: 100%;\n  overflow: hidden;\n  text-overflow: ellipsis;\n  white-space: nowrap;\n}\n\n.unit-select-button small {\n  color: var(--muted);\n}\n\n.mini-field {\n  display: flex;\n  flex-direction: column;\n  gap: 3px;\n  color: var(--muted);\n  font-size: 9px;\n  text-transform: uppercase;\n}\n\n.mini-field input {\n  min-height: 34px;\n  padding: 6px 8px;\n}\n\n.weight-input-row {\n  display: grid;\n  grid-template-columns: minmax(72px, 1fr) 58px;\n  gap: 6px;\n  align-items: center;\n}\n\n.slot-percent {\n  color: var(--cyan);\n  font-family: \"Cascadia Mono\", monospace;\n  font-size: 11px;\n  text-align: right;\n}\n\n.entry-advanced {\n  min-width: 0;\n  border: 1px solid var(--line);\n  border-radius: 7px;\n  padding: 7px 9px;\n  color: var(--muted);\n  font-size: 10px;\n  line-height: 1.45;\n}\n\n.entry-advanced summary {\n  color: #b7c8d6;\n  cursor: pointer;\n}\n\n.entry-advanced span {\n  display: block;\n  margin-top: 4px;\n}\n\n.remove-entry,\n.icon-button {\n  width: 36px;\n  min-height: 36px;\n  padding: 0;\n  border-radius: 50%;\n}\n\n.rules-heading {\n  margin-top: 24px;\n}\n\n.rules-grid {\n  display: grid;\n  grid-template-columns: repeat(4, minmax(0, 1fr));\n  gap: 8px;\n}\n\n.rule-card {\n  border: 1px solid var(--line);\n  border-radius: 9px;\n  padding: 10px;\n  background: rgb(5 10 16 / 46%);\n}\n\n.rule-card h4 {\n  margin: 0 0 8px;\n  color: var(--cyan);\n}\n\n.rule-card label {\n  display: flex;\n  flex-direction: column;\n  gap: 4px;\n  margin-top: 7px;\n  color: var(--muted);\n  font-size: 10px;\n}\n\n.rule-card .inline-check {\n  flex-direction: row;\n  align-items: center;\n}\n\n.notice {\n  border: 1px solid var(--line);\n  border-radius: 8px;\n  padding: 10px 12px;\n  color: #b7c8d6;\n  background: rgb(255 255 255 / 3%);\n  font-size: 12px;\n  line-height: 1.55;\n}\n\n.notice.error {\n  margin-bottom: 14px;\n  color: #ffdadd;\n  border-color: rgb(255 100 112 / 48%);\n  background: rgb(255 100 112 / 8%);\n}\n\n.footerbar {\n  height: 58px;\n  display: flex;\n  align-items: center;\n  justify-content: space-between;\n  gap: 20px;\n  padding: 0 18px;\n  border-top: 1px solid var(--line);\n  background: #090d13;\n}\n\n.footerbar > div:first-child {\n  min-width: 0;\n  flex: 1;\n}\n\n.footer-actions {\n  flex-shrink: 0;\n}\n\n#catalog-path {\n  display: block;\n  max-width: 100%;\n  overflow: hidden;\n  text-overflow: ellipsis;\n  white-space: nowrap;\n}\n\ndialog {\n  width: min(720px, calc(100vw - 48px));\n  max-height: calc(100vh - 48px);\n  border: 1px solid #395068;\n  border-radius: 14px;\n  padding: 0;\n  color: var(--text);\n  background: #101720;\n  box-shadow: var(--shadow);\n}\n\ndialog::backdrop {\n  background: rgb(1 4 8 / 74%);\n  backdrop-filter: blur(4px);\n}\n\n.dialog-card {\n  display: flex;\n  flex-direction: column;\n  max-height: calc(100vh - 50px);\n}\n\n.dialog-heading,\n.dialog-actions {\n  padding: 17px 18px;\n  border-bottom: 1px solid var(--line);\n}\n\n.dialog-actions {\n  display: flex;\n  justify-content: flex-end;\n  gap: 8px;\n  border-top: 1px solid var(--line);\n  border-bottom: 0;\n}\n\n.dialog-body {\n  min-height: 0;\n  overflow: auto;\n  padding: 18px;\n}\n\n.form-stack {\n  display: flex;\n  flex-direction: column;\n  gap: 13px;\n}\n\n.unit-dialog-card {\n  width: min(980px, calc(100vw - 48px));\n}\n\n#unit-dialog {\n  width: min(980px, calc(100vw - 48px));\n}\n\n.unit-grid {\n  display: grid;\n  grid-template-columns: repeat(4, minmax(0, 1fr));\n  gap: 8px;\n}\n\n.unit-card {\n  min-width: 0;\n  height: auto;\n  display: grid;\n  grid-template-columns: 64px 1fr;\n  gap: 9px;\n  padding: 8px;\n  text-align: left;\n}\n\n.unit-card .unit-thumb,\n.unit-card .unit-placeholder {\n  width: 64px;\n  height: 64px;\n}\n\n.unit-card-copy {\n  min-width: 0;\n  display: flex;\n  flex-direction: column;\n  justify-content: center;\n}\n\n.unit-card-copy strong,\n.unit-card-copy small {\n  overflow: hidden;\n  text-overflow: ellipsis;\n  white-space: nowrap;\n}\n\n.unit-card-copy small {\n  margin-top: 5px;\n  color: var(--muted);\n}\n\n.result-content {\n  white-space: pre-wrap;\n  overflow-wrap: anywhere;\n  color: #c8d7e3;\n  font-family: \"Cascadia Mono\", \"Microsoft YaHei UI\", monospace;\n  font-size: 12px;\n  line-height: 1.65;\n}\n\n.toast {\n  position: fixed;\n  right: 20px;\n  bottom: 76px;\n  z-index: 100;\n  max-width: 430px;\n  border: 1px solid rgb(70 219 232 / 45%);\n  border-radius: 9px;\n  padding: 12px 15px;\n  color: #dffcff;\n  background: #10232a;\n  box-shadow: var(--shadow);\n}\n\n@media (max-width: 1180px) {\n  .form-grid {\n    grid-template-columns: repeat(2, minmax(0, 1fr));\n  }\n\n  .rules-grid {\n    grid-template-columns: repeat(2, minmax(0, 1fr));\n  }\n\n  .entry-row {\n    grid-template-columns: 34px 52px minmax(160px, 1.4fr) minmax(140px, .8fr) minmax(118px, .7fr) 36px;\n  }\n\n  .unit-grid {\n    grid-template-columns: repeat(3, minmax(0, 1fr));\n  }\n}\n\n.module-body{padding:0!important;max-width:none!important;margin:0!important} button,input,select,textarea{font-family:inherit} "+"</style><div class=\"module-body\"><meta name=\"csrf-token\" content=\""+ctx.token+"\">"+"\n  <header class=\"topbar\">\n    <div>\n      <p class=\"eyebrow\">CAPSULE RUNTIME CONFIG</p>\n      <h1>SDGO 扭蛋机配置台</h1>\n    </div>\n    <div class=\"topbar-status\">\n      <span id=\"save-state\" class=\"status-pill clean\">已同步</span>\n      <span id=\"revision\" class=\"mono muted\">revision —</span>\n    </div>\n  </header>\n\n  <div class=\"workspace\">\n    <aside class=\"sidebar\">\n      <div class=\"sidebar-actions\">\n        <button id=\"new-machine\" class=\"primary\">＋ 新建机台</button>\n        <button id=\"import-button\">导入配置</button>\n        <input id=\"import-file\" type=\"file\" accept=\"application/json,.json\" hidden>\n        <a class=\"button-link\" href=\"/api/providers/capsules/api/catalog/download\">下载目录 JSON</a>\n      </div>\n\n      <div class=\"tabs\" role=\"tablist\" aria-label=\"机台类型\">\n        <button class=\"tab active\" data-kind=\"fixed_machines\" role=\"tab\">固定扭蛋</button>\n        <button class=\"tab\" data-kind=\"custom_machines\" role=\"tab\">自定义扭蛋</button>\n      </div>\n\n      <label class=\"search-box\">\n        <span>搜索</span>\n        <input id=\"machine-search\" type=\"search\" placeholder=\"名称或 Machine ID\">\n      </label>\n\n      <div class=\"filter-row\">\n        <label><input type=\"radio\" name=\"machine-filter\" value=\"all\" checked> 全部</label>\n        <label><input type=\"radio\" name=\"machine-filter\" value=\"enabled\"> 启用</label>\n        <label><input type=\"radio\" name=\"machine-filter\" value=\"retired\"> 下架</label>\n      </div>\n\n      <div id=\"machine-count\" class=\"list-caption\">正在读取…</div>\n      <nav id=\"machine-list\" class=\"machine-list\" aria-label=\"机台列表\"></nav>\n    </aside>\n\n    <main class=\"main-panel\">\n      <section id=\"empty-state\" class=\"empty-state\">\n        <div class=\"empty-mark\">◉</div>\n        <h2>选择一台扭蛋机</h2>\n        <p>可以修改价格、机体、权重与显示位置，也可以从原生 Pattern 或现有机台复制新建。</p>\n      </section>\n\n      <form id=\"editor\" class=\"editor hidden\">\n        <div class=\"editor-heading\">\n          <div>\n            <div class=\"editor-badges\">\n              <span id=\"editor-kind\" class=\"badge\"></span>\n              <span id=\"editor-location\" class=\"badge accent\"></span>\n              <span id=\"editor-owner\" class=\"badge\"></span>\n            </div>\n            <h2 id=\"editor-title\">机台</h2>\n            <p id=\"editor-subtitle\" class=\"muted\"></p>\n          </div>\n          <div class=\"editor-actions\">\n            <button id=\"restore-machine\" type=\"button\" class=\"success hidden\">恢复上架</button>\n            <button id=\"retire-machine\" type=\"button\" class=\"danger\">下架并保留 ID</button>\n            <button id=\"save-machine\" type=\"submit\" class=\"primary\">保存配置</button>\n          </div>\n        </div>\n\n        <div id=\"editor-error\" class=\"notice error hidden\" role=\"alert\"></div>\n\n        <section class=\"panel\">\n          <div class=\"form-grid\">\n            <label>\n              <span>Machine ID</span>\n              <input id=\"field-id\" type=\"number\" min=\"1\" required>\n            </label>\n            <label>\n              <span>配置顺序</span>\n              <input id=\"field-position\" type=\"number\" min=\"1\" required>\n              <small id=\"position-help\">固定机在游戏中倒序显示；上方显示实际页码和格位。</small>\n            </label>\n            <label class=\"span-2\">\n              <span>显示名称</span>\n              <input id=\"field-name\" type=\"text\" maxlength=\"100\" required>\n            </label>\n            <label>\n              <span>价格</span>\n              <input id=\"field-price\" type=\"number\" min=\"0\" required>\n            </label>\n            <label>\n              <span>货币</span>\n              <select id=\"field-currency\"></select>\n            </label>\n            <div class=\"read-only-field\">\n              <span>来源</span>\n              <strong id=\"field-owner-label\">—</strong>\n            </div>\n            <div class=\"read-only-field\">\n              <span>抽取方式</span>\n              <strong id=\"field-policy-label\">—</strong>\n            </div>\n            <label class=\"check-field\">\n              <input id=\"field-enabled\" type=\"checkbox\">\n              <span>启用机台</span>\n            </label>\n          </div>\n        </section>\n\n        <section id=\"fixed-editor\" class=\"panel\">\n          <div class=\"section-heading\">\n            <div>\n              <p class=\"eyebrow\">FIXED CAPSULE</p>\n              <h3>六格机体与权重</h3>\n            </div>\n            <div id=\"weight-total\" class=\"weight-total\">合计 0 / 1000</div>\n          </div>\n          <p class=\"section-note\">固定机当前运行时仅支持 GP。Machine ID 与显示位置分开保存；每页 8 台。</p>\n          <div id=\"fixed-slots\" class=\"entry-table\"></div>\n        </section>\n\n        <section id=\"custom-editor\" class=\"panel hidden\">\n          <div class=\"section-heading\">\n            <div>\n              <p class=\"eyebrow\">CUSTOM CAPSULE</p>\n              <h3>候选机体</h3>\n            </div>\n            <button id=\"add-candidate\" type=\"button\">＋ 添加候选</button>\n          </div>\n          <p class=\"section-note\">原生规则是 6 个基础格与 2 个扩展格。候选 SR 数量不等于槽位数量；每个候选保留自己的权重、累积、加价和重置参数。</p>\n          <p class=\"section-note\">保留原生基础候选。新增候选只允许 SR，或完全没有设计图的 SS；有设计图的 SS 不加入自定义池。</p>\n          <div id=\"custom-candidates\" class=\"entry-table\"></div>\n\n          <div class=\"section-heading rules-heading\">\n            <div>\n              <p class=\"eyebrow\">SLOT RULES</p>\n              <h3>八格选择限制</h3>\n            </div>\n          </div>\n          <div id=\"slot-rules\" class=\"rules-grid\"></div>\n        </section>\n      </form>\n    </main>\n  </div>\n\n  <footer class=\"footerbar\">\n    <div>\n      <span id=\"catalog-path\" class=\"mono muted\"></span>\n    </div>\n    <div class=\"footer-actions\">\n      <button id=\"initialize-button\">初始化缺失取得路线</button>\n      <button id=\"compile-button\">编译客户端 Overlay</button>\n      <button id=\"export-button\" class=\"primary\">导出客户端 / 服务端配置</button>\n      <button id=\"apply-button\" class=\"success\">导入到游戏</button>\n    </div>\n  </footer>\n\n  <dialog id=\"new-dialog\">\n    <div class=\"dialog-card\">\n      <div class=\"dialog-heading\">\n        <div>\n          <p class=\"eyebrow\">NEW MACHINE</p>\n          <h2>从模板新建机台</h2>\n        </div>\n        <button class=\"icon-button\" type=\"button\" data-close-dialog=\"new-dialog\" aria-label=\"关闭\">×</button>\n      </div>\n      <div class=\"dialog-body form-stack\">\n        <label>\n          <span>创建方式</span>\n          <select id=\"new-method\"></select>\n        </label>\n        <label>\n          <span>Pattern / 来源机台</span>\n          <select id=\"new-template\"></select>\n        </label>\n        <div id=\"new-template-note\" class=\"notice\"></div>\n        <div class=\"form-grid\">\n          <label>\n            <span>新 Machine ID</span>\n            <input id=\"new-id\" type=\"number\" min=\"1\" required>\n          </label>\n          <label>\n            <span>插入位置</span>\n            <input id=\"new-position\" type=\"number\" min=\"1\" required>\n          </label>\n          <label class=\"span-2\">\n            <span>显示名称</span>\n            <input id=\"new-name\" type=\"text\" maxlength=\"100\" required>\n          </label>\n        </div>\n      </div>\n      <div class=\"dialog-actions\">\n        <button type=\"button\" data-close-dialog=\"new-dialog\">取消</button>\n        <button id=\"create-draft\" type=\"button\" class=\"primary\">打开新机台草稿</button>\n      </div>\n    </div>\n  </dialog>\n\n  <dialog id=\"unit-dialog\">\n    <div class=\"dialog-card unit-dialog-card\">\n      <div class=\"dialog-heading\">\n        <div>\n          <p class=\"eyebrow\">UNIT PICKER</p>\n          <h2>选择机体</h2>\n        </div>\n        <button class=\"icon-button\" type=\"button\" data-close-dialog=\"unit-dialog\" aria-label=\"关闭\">×</button>\n      </div>\n      <div class=\"dialog-body\">\n        <label class=\"search-box large\">\n          <span>搜索机体</span>\n          <input id=\"unit-search\" type=\"search\" placeholder=\"机体名称、ID 或等级\">\n        </label>\n        <div id=\"unit-result-count\" class=\"list-caption\"></div>\n        <div id=\"unit-grid\" class=\"unit-grid\"></div>\n      </div>\n    </div>\n  </dialog>\n\n  <dialog id=\"result-dialog\">\n    <div class=\"dialog-card result-card\">\n      <div class=\"dialog-heading\">\n        <div>\n          <p class=\"eyebrow\">RESULT</p>\n          <h2 id=\"result-title\">操作完成</h2>\n        </div>\n        <button class=\"icon-button\" type=\"button\" data-close-dialog=\"result-dialog\" aria-label=\"关闭\">×</button>\n      </div>\n      <div id=\"result-content\" class=\"dialog-body result-content\"></div>\n      <div class=\"dialog-actions\">\n        <button type=\"button\" class=\"primary\" data-close-dialog=\"result-dialog\">知道了</button>\n      </div>\n    </div>\n  </dialog>\n\n  <div id=\"toast\" class=\"toast hidden\" role=\"status\"></div>\n  \n"+"</div>";
 const document=new Proxy(shadow,{get(t,k){if(k==="createElement")return globalThis.document.createElement.bind(globalThis.document);const v=t[k];return typeof v==="function"?v.bind(t):v;}});
 const fetch=ctx.fetch, setInterval=ctx.setInterval;
 const window={confirm:globalThis.confirm.bind(globalThis),prompt:globalThis.prompt.bind(globalThis),setTimeout:globalThis.setTimeout.bind(globalThis),clearTimeout:globalThis.clearTimeout.bind(globalThis),addEventListener:ctx.onWindow};
"use strict";

const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
const csrfToken = $('meta[name="csrf-token"]').content;

const state = {
  bootstrap: null,
  revision: null,
  kind: "fixed_machines",
  selected: null,
  detail: null,
  dirty: false,
  unitTarget: null,
  unitMap: new Map(),
  newTemplates: [],
};

function escapeHtml(value) {
  return String(value ?? "")
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#039;");
}

function deepClone(value) {
  return JSON.parse(JSON.stringify(value));
}

function kindLabel(kind) {
  return kind === "fixed_machines" ? "固定扭蛋" : "自定义扭蛋";
}

function ownerLabel(owner) {
  return {
    native: "原生机台",
    initialization: "初始化补齐",
    mod: "Mod 新增",
  }[owner] || "其他来源";
}

function policyLabel(policy) {
  return {
    native_sequence: "沿用原生顺序抽取",
    independent_weighted: "按配置概率独立抽取",
    native_mileage: "沿用原生累积抽取",
  }[policy] || "由运行时处理";
}

function currencyLabel(kind, value) {
  if (kind === "fixed_machines") return "GP";
  return Number(value) === 1 ? "Coin" : "GP";
}

function locationText(kind, locationOrPosition) {
  const location = typeof locationOrPosition === "number"
    ? { position: locationOrPosition }
    : locationOrPosition;
  if (!location) return "未定位";
  if (kind === "fixed_machines") {
    const page = location.page ?? Math.floor((location.position - 1) / 8) + 1;
    const slot = location.slot ?? ((location.position - 1) % 8) + 1;
    return `第 ${location.position} 位 · 第 ${page} 页第 ${slot} 格`;
  }
  return `第 ${location.position} 位`;
}

function setSaveState(mode, text) {
  const element = $("#save-state");
  element.className = `status-pill ${mode}`;
  element.textContent = text;
}

function setDirty(value) {
  state.dirty = value;
  setSaveState(value ? "dirty" : "clean", value ? "有未保存修改" : "已同步");
}

function showToast(message, timeout = 3600) {
  const toast = $("#toast");
  toast.textContent = message;
  toast.classList.remove("hidden");
  window.clearTimeout(showToast.timer);
  showToast.timer = window.setTimeout(() => toast.classList.add("hidden"), timeout);
}

function showEditorError(message) {
  const box = $("#editor-error");
  box.textContent = message;
  box.classList.toggle("hidden", !message);
}

function showResult(title, lines) {
  $("#result-title").textContent = title;
  $("#result-content").textContent = Array.isArray(lines) ? lines.join("\n") : String(lines);
  $("#result-dialog").showModal();
}

async function apiGet(path) {
  const response = await fetch(path, {
    credentials: "same-origin",
    headers: { Accept: "application/json" },
  });
  const body = await response.json().catch(() => ({ error: `HTTP ${response.status}` }));
  if (!response.ok) throw new Error(body.error || `HTTP ${response.status}`);
  return body;
}

async function apiPost(path, payload) {
  const response = await fetch(path, {
    method: "POST",
    credentials: "same-origin",
    headers: {
      Accept: "application/json",
      "Content-Type": "application/json",
      "X-Capsule-Token": csrfToken,
    },
    body: JSON.stringify(payload),
  });
  const body = await response.json().catch(() => ({ error: `HTTP ${response.status}` }));
  if (!response.ok) {
    const error = new Error(body.error || `HTTP ${response.status}`);
    error.status = response.status;
    throw error;
  }
  return body;
}

async function loadBootstrap(selectAfter = null) {
  const data = await apiGet("/api/providers/capsules/api/bootstrap");
  state.bootstrap = data;
  state.revision = data.catalog_revision;
  state.unitMap = new Map(data.units.map((unit) => [unit.id, unit]));
  $("#revision").textContent = `revision ${data.catalog_revision.slice(0, 12)}`;
  $("#catalog-path").textContent = data.catalog_path;
  $("#catalog-path").title = data.catalog_path;
  const initializeButton = $("#initialize-button");
  initializeButton.disabled = !data.capabilities.initialize;
  initializeButton.title = data.capabilities.initialize
    ? "根据已配置的快照和客户端补齐取得路线"
    : "启动 UI 时同时提供 --snapshot 与 --client 后开放";
  const compileButton = $("#compile-button");
  compileButton.disabled = !data.capabilities.compile;
  compileButton.title = data.capabilities.compile
    ? "生成独立版本化客户端 Overlay；不会自动安装"
    : "启动 UI 时同时提供 --client 与 --lookup 后开放";
  const applyButton = $("#apply-button");
  applyButton.disabled = !data.capabilities.apply;
  applyButton.title = data.capabilities.apply
    ? "将当前 revision 导入启动 UI 时预配置的客户端和服务端"
    : "启动 UI 时预配置安装客户端与框架目录后开放";
  renderMachineList();
  if (selectAfter) {
    await selectMachine(selectAfter.kind, selectAfter.id, true);
  } else if (state.selected) {
    const exists = data.machines[state.selected.kind]
      .some((machine) => machine.id === state.selected.id);
    if (exists) await selectMachine(state.selected.kind, state.selected.id, true);
    else clearEditor();
  }
}

function filteredMachines() {
  const query = $("#machine-search").value.trim().toLocaleLowerCase();
  const filter = $('input[name="machine-filter"]:checked').value;
  return state.bootstrap.machines[state.kind].filter((machine) => {
    const matchesQuery = !query
      || String(machine.id).includes(query)
      || machine.name.toLocaleLowerCase().includes(query);
    const matchesFilter = filter === "all"
      || (filter === "enabled" && machine.enabled && !machine.retired)
      || (filter === "retired" && (machine.retired || !machine.enabled));
    return matchesQuery && matchesFilter;
  });
}

function renderMachineList() {
  if (!state.bootstrap) return;
  $$(".tab").forEach((tab) => {
    tab.classList.toggle("active", tab.dataset.kind === state.kind);
  });
  const machines = filteredMachines();
  $("#machine-count").textContent =
    `${machines.length} / ${state.bootstrap.machines[state.kind].length} 台`;
  const container = $("#machine-list");
  container.innerHTML = machines.map((machine) => {
    const active = state.selected
      && state.selected.kind === state.kind
      && state.selected.id === machine.id;
    const status = machine.retired || !machine.enabled ? "已下架" : "启用";
    const nativeLocation = machine.native_location ?? machine.location;
    const fixedLocation = nativeLocation
      ? `P${nativeLocation.page ?? "?"}-${nativeLocation.slot ?? "?"}`
      : "OFF";
    return `
      <button class="machine-card ${active ? "active" : ""} ${status === "已下架" ? "retired" : ""}"
              type="button" data-machine-id="${machine.id}">
        <span class="machine-position">
          ${machine.position}
          <small>${state.kind === "fixed_machines" ? fixedLocation : "POSITION"}</small>
        </span>
        <span class="machine-copy">
          <strong>${escapeHtml(machine.name)}</strong>
          <small>ID ${machine.id} · ${escapeHtml(ownerLabel(machine.owner))} · ${status}</small>
        </span>
        <span class="machine-price">${machine.price} ${currencyLabel(state.kind, machine.currency)}</span>
      </button>`;
  }).join("");
  $$("[data-machine-id]", container).forEach((button) => {
    button.addEventListener("click", () => selectMachine(state.kind, Number(button.dataset.machineId)));
  });
}

function confirmDiscard() {
  return !state.dirty || window.confirm("当前有未保存修改。放弃这些修改吗？");
}

async function selectMachine(kind, id, force = false) {
  if (!force && !confirmDiscard()) return;
  try {
    const detail = await apiGet(`/api/providers/capsules/api/machine?kind=${encodeURIComponent(kind)}&id=${id}`);
    state.kind = kind;
    state.selected = { kind, id };
    state.detail = { ...detail, machine: deepClone(detail.machine), isNew: false };
    state.revision = detail.catalog_revision;
    renderMachineList();
    renderEditor();
    setDirty(false);
  } catch (error) {
    showToast(error.message);
  }
}

function clearEditor() {
  state.selected = null;
  state.detail = null;
  $("#editor").classList.add("hidden");
  $("#empty-state").classList.remove("hidden");
  renderMachineList();
  setDirty(false);
}

function unitVisual(unit) {
  if (unit?.has_asset) {
    return `<img class="unit-thumb" src="${escapeHtml(unit.asset_url)}" alt="">`;
  }
  return '<span class="unit-placeholder" aria-hidden="true">◇</span>';
}

function unitButton(unit, targetType, index) {
  const name = unit?.name ?? "选择机体";
  const subtitle = unit
    ? `ID ${unit.id} · ${unit.grade || "?"}`
    : "尚未选择";
  return `
    <button class="unit-select-button" type="button"
            data-pick-unit="${targetType}" data-entry-index="${index}">
      <strong>${escapeHtml(name)}</strong>
      <small>${escapeHtml(subtitle)}</small>
    </button>`;
}

function renderFixedSlots(machine) {
  const container = $("#fixed-slots");
  container.innerHTML = machine.slots.map((slot, index) => {
    const unit = state.unitMap.get(Number(slot.unit_id));
    return `
      <div class="entry-row fixed-row" data-entry-index="${index}" data-unit-id="${slot.unit_id}">
        <div class="entry-index">${index + 1}</div>
        ${unitVisual(unit)}
        ${unitButton(unit, "slot", index)}
        <label class="mini-field">概率权重
          <span class="weight-input-row">
            <input class="slot-weight" type="number" min="1" step="1" value="${slot.weight}" required>
            <output class="slot-percent">${(Number(slot.weight) / 10).toFixed(1)}%</output>
          </span>
        </label>
        <details class="entry-advanced">
          <summary>高级信息</summary>
          <span>原生抽取组 ${slot.native_draw_group ?? unit?.rank ?? 0}</span>
          <span>SPR 引用 ${(slot.sprites || []).length} 条</span>
        </details>
        <span class="badge">固定</span>
      </div>`;
  }).join("");
  updateWeightTotal();
}

function renderCustomCandidates(machine) {
  const container = $("#custom-candidates");
  container.innerHTML = machine.candidates.map((candidate, index) => {
    const unit = state.unitMap.get(Number(candidate.unit_id));
    return `
      <div class="entry-row custom-row" data-entry-index="${index}" data-unit-id="${candidate.unit_id}">
        <div class="entry-index">${index + 1}</div>
        ${unitVisual(unit)}
        ${unitButton(unit, "candidate", index)}
        <label class="mini-field">basic weight
          <input class="candidate-weight" type="number" min="1" step="1"
                 value="${candidate.basic_weight}" required>
        </label>
        <label class="mini-field">mileage
          <input class="candidate-mileage" type="number" min="0" step="any"
                 value="${candidate.mileage}" required>
        </label>
        <label class="mini-field">extra price
          <input class="candidate-extra" type="number" min="0" step="1"
                 value="${candidate.extra_price}" required>
        </label>
        <label class="mini-field">unit id
          <input type="text" value="${candidate.unit_id}" disabled>
        </label>
        <label class="mini-field">重置累积
          <input class="candidate-reset" type="checkbox" ${candidate.reset_mileage ? "checked" : ""}>
        </label>
        <button class="remove-entry" type="button" data-remove-candidate="${index}" aria-label="移除候选">×</button>
      </div>`;
  }).join("");
}

function renderSlotRules(machine) {
  $("#slot-rules").innerHTML = machine.slot_rules.map((rule, index) => `
    <div class="rule-card" data-rule-index="${index}">
      <h4>槽位 ${rule.slot}</h4>
      <label>最高 Rank
        <input class="rule-rank" type="number" min="1" max="4" step="1"
               value="${rule.rank_max}" required>
      </label>
      <label>允许 Subgrade（逗号分隔）
        <input class="rule-subgrades" type="text"
               value="${escapeHtml((rule.subgrades || []).join(","))}" required>
      </label>
      <label class="inline-check">
        <input class="rule-extension" type="checkbox" ${rule.extension ? "checked" : ""}>
        扩展格
      </label>
    </div>`).join("");
}

function renderEditor() {
  const detail = state.detail;
  if (!detail) return clearEditor();
  const machine = detail.machine;
  const kind = detail.kind;
  $("#empty-state").classList.add("hidden");
  $("#editor").classList.remove("hidden");
  $("#editor-kind").textContent = kindLabel(kind);
  $("#editor-location").textContent = detail.isNew
    ? `草稿 · ${locationText(kind, detail.position)}`
    : `配置顺序第 ${detail.position} 位 · 游戏中${locationText(
      kind,
      detail.native_location ?? detail.location
    )}`;
  $("#editor-owner").textContent = `来源：${ownerLabel(machine.owner)}`;
  $("#editor-title").textContent = machine.name || `Machine ${machine.id}`;
  $("#editor-subtitle").textContent =
    `Machine ID ${machine.id} · ${machine.enabled ? "启用" : "停用"} · ${policyLabel(machine.draw_policy)}`;
  $("#field-id").value = machine.id;
  $("#field-id").disabled = !detail.isNew;
  $("#field-position").value = detail.position;
  $("#field-position").max =
    state.bootstrap.machines[kind].length + (detail.isNew ? 1 : 0);
  $("#field-name").value = machine.name || "";
  $("#field-price").value = machine.price ?? 0;
  $("#field-owner-label").textContent = ownerLabel(machine.owner);
  $("#field-policy-label").textContent = policyLabel(machine.draw_policy);
  $("#field-enabled").checked = Boolean(machine.enabled);

  const currency = $("#field-currency");
  if (kind === "fixed_machines") {
    currency.innerHTML = '<option value="GP">GP（当前固定机唯一支持项）</option>';
    currency.value = "GP";
    currency.disabled = true;
  } else {
    currency.innerHTML = '<option value="0">0 · GP</option><option value="1">1 · Coin</option>';
    currency.value = String(machine.currency_type ?? 0);
    currency.disabled = false;
  }

  const retired = Boolean(machine.retired);
  $("#retire-machine").classList.toggle("hidden", retired || detail.isNew);
  $("#restore-machine").classList.toggle("hidden", !retired || detail.isNew);
  $("#fixed-editor").classList.toggle("hidden", kind !== "fixed_machines");
  $("#custom-editor").classList.toggle("hidden", kind !== "custom_machines");
  if (kind === "fixed_machines") renderFixedSlots(machine);
  else {
    renderCustomCandidates(machine);
    renderSlotRules(machine);
  }
  showEditorError("");
}

function readInt(element, label, minimum = 0) {
  const value = Number(element.value);
  if (!Number.isInteger(value) || value < minimum) {
    throw new Error(`${label} 必须是大于等于 ${minimum} 的整数`);
  }
  return value;
}

function readNumber(element, label, minimum = 0) {
  const value = Number(element.value);
  if (!Number.isFinite(value) || value < minimum) {
    throw new Error(`${label} 必须是大于等于 ${minimum} 的数字`);
  }
  return value;
}

function readMachineFromForm() {
  if (!state.detail) throw new Error("没有打开机台");
  const machine = deepClone(state.detail.machine);
  machine.id = readInt($("#field-id"), "Machine ID", 1);
  machine.name = $("#field-name").value.trim();
  if (!machine.name) throw new Error("显示名称不能为空");
  machine.price = readInt($("#field-price"), "价格", 0);
  machine.enabled = $("#field-enabled").checked;

  if (state.detail.kind === "fixed_machines") {
    machine.currency = "GP";
    machine.slots = $$(".fixed-row").map((row, index) => {
      const previous = machine.slots[index] || {};
      return {
        ...previous,
        unit_id: Number(row.dataset.unitId),
        weight: readInt($(".slot-weight", row), `槽位 ${index + 1} 权重`, 1),
      };
    });
    if (machine.slots.length !== 6) throw new Error("固定机必须有 6 个槽位");
    const total = machine.slots.reduce((sum, slot) => sum + slot.weight, 0);
    if (total !== 1000) throw new Error(`固定机权重合计必须为 1000，当前为 ${total}`);
    if (new Set(machine.slots.map((slot) => slot.unit_id)).size !== 6) {
      throw new Error("固定机的 6 个机体不能重复");
    }
  } else {
    machine.currency_type = readInt($("#field-currency"), "货币类型", 0);
    if (![0, 1].includes(machine.currency_type)) throw new Error("货币类型只允许 0 或 1");
    machine.candidates = $$(".custom-row").map((row, index) => ({
      unit_id: Number(row.dataset.unitId),
      basic_weight: readInt($(".candidate-weight", row), `候选 ${index + 1} 权重`, 1),
      mileage: readNumber($(".candidate-mileage", row), `候选 ${index + 1} 累积`, 0),
      extra_price: readInt($(".candidate-extra", row), `候选 ${index + 1} 加价`, 0),
      reset_mileage: $(".candidate-reset", row).checked,
    }));
    if (new Set(machine.candidates.map((candidate) => candidate.unit_id)).size
        !== machine.candidates.length) {
      throw new Error("自定义机候选机体不能重复");
    }
    machine.slot_rules = $$(".rule-card").map((card, index) => {
      const subgrades = $(".rule-subgrades", card).value
        .split(",")
        .map((value) => Number(value.trim()))
        .filter((value) => Number.isInteger(value));
      if (!subgrades.length) throw new Error(`槽位 ${index + 1} 至少需要一个 Subgrade`);
      return {
        slot: index + 1,
        rank_max: readInt($(".rule-rank", card), `槽位 ${index + 1} Rank`, 1),
        subgrades,
        extension: $(".rule-extension", card).checked,
      };
    });
  }
  return machine;
}

function captureDraft() {
  const machine = readMachineFromForm();
  state.detail.machine = machine;
  state.detail.position = readInt($("#field-position"), "配置顺序", 1);
}

function updateWeightTotal() {
  const inputs = $$(".slot-weight");
  const values = inputs.map((input) => Number(input.value) || 0);
  inputs.forEach((input, index) => {
    const output = input.closest(".fixed-row")?.querySelector(".slot-percent");
    if (output) output.textContent = `${(values[index] / 10).toFixed(1)}%`;
  });
  const total = values.reduce((sum, value) => sum + value, 0);
  const element = $("#weight-total");
  element.textContent = `合计 ${total} / 1000 · ${(total / 10).toFixed(1)}%`;
  element.classList.toggle("valid", total === 1000);
}

function openUnitPicker(target, index) {
  try {
    captureDraft();
  } catch (error) {
    showEditorError(error.message);
    return;
  }
  state.unitTarget = { target, index };
  $("#unit-search").value = "";
  renderUnitGrid();
  $("#unit-dialog").showModal();
  $("#unit-search").focus();
}

function renderUnitGrid() {
  const query = $("#unit-search").value.trim().toLocaleLowerCase();
  const matches = state.bootstrap.units.filter((unit) => {
    if (state.unitTarget?.target === "candidate" && unit.can_add_custom === false) return false;
    const text = `${unit.id} ${unit.name} ${unit.grade}`.toLocaleLowerCase();
    return !query || text.includes(query);
  }).slice(0, 120);
  $("#unit-result-count").textContent =
    `显示 ${matches.length} 台${matches.length === 120 ? "（继续输入可缩小范围）" : ""}`;
  $("#unit-grid").innerHTML = matches.map((unit) => `
    <button class="unit-card" type="button" data-unit-id="${unit.id}">
      ${unitVisual(unit)}
      <span class="unit-card-copy">
        <strong>${escapeHtml(unit.name)}</strong>
        <small>ID ${unit.id} · ${escapeHtml(unit.grade || "?")}${unit.eligible ? "" : " · 不可初始化"}</small>
      </span>
    </button>`).join("");
  $$("[data-unit-id]", $("#unit-grid")).forEach((button) => {
    button.addEventListener("click", () => chooseUnit(Number(button.dataset.unitId)));
  });
}

function chooseUnit(unitId) {
  const unit = state.unitMap.get(unitId);
  if (!unit || !state.unitTarget) return;
  const { target, index } = state.unitTarget;
  if (target === "candidate" && unit.can_add_custom === false) {
    showToast("新增候选仅允许 SR 或没有设计图的 SS");
    return;
  }
  if (target === "slot") {
    const slot = state.detail.machine.slots[index];
    slot.unit_id = unitId;
    slot.sprites = deepClone(unit.capsule_sprites || []);
    slot.native_draw_group = unit.rank ?? slot.native_draw_group ?? 0;
  } else {
    state.detail.machine.candidates[index].unit_id = unitId;
  }
  $("#unit-dialog").close();
  renderEditor();
  setDirty(true);
}

function nextMachineId(kind) {
  const ids = state.bootstrap.machines[kind].map((machine) => machine.id);
  if (!ids.length) return kind === "fixed_machines" ? 101 : 1;
  if (kind === "fixed_machines") {
    const used = new Set(ids);
    let candidate = (Math.floor(Math.max(...ids) / 100) + 1) * 100 + 1;
    while (used.has(candidate) || used.has(candidate + 1)) candidate += 100;
    return candidate;
  }
  return Math.max(...ids) + 1;
}

function populateNewDialog() {
  const kind = state.kind;
  const method = $("#new-method");
  method.innerHTML = kind === "fixed_machines"
    ? '<option value="pattern">使用原生 Pattern</option><option value="clone">复制现有机台</option>'
    : '<option value="clone">复制现有自定义机</option>';
  $("#new-id").value = nextMachineId(kind);
  $("#new-position").value = state.bootstrap.machines[kind].length + 1;
  $("#new-position").max = state.bootstrap.machines[kind].length + 1;
  $("#new-name").value = kind === "fixed_machines" ? "New Capsule" : "New Custom Capsule";
  refreshNewTemplates();
  $("#new-dialog").showModal();
}

function refreshNewTemplates() {
  const method = $("#new-method").value;
  const kind = state.kind;
  const templates = [];
  if (kind === "fixed_machines" && method === "pattern") {
    state.bootstrap.patterns.forEach((pattern, patternIndex) => {
      (pattern.weight_variants || []).forEach((weights, variantIndex) => {
        templates.push({
          value: `pattern:${patternIndex}:${variantIndex}`,
          machineId: pattern.machines[0],
          weights,
          label: `${pattern.grades.join(" / ")} · ${weights.join("-")} · 来源 #${pattern.machines[0]}`,
          note: `精确等级排列：${pattern.grades.join(" / ")}；权重合计 ${weights.reduce((a, b) => a + b, 0)}。`,
        });
      });
    });
  } else {
    state.bootstrap.machines[kind].forEach((machine) => {
      templates.push({
        value: `clone:${machine.id}`,
        machineId: machine.id,
        label: `#${machine.id} · ${machine.name} · ${locationText(kind, machine.location || machine.position)}`,
        note: `复制现有机台 #${machine.id} 的完整规则，再以新 ID 保存。`,
      });
    });
  }
  state.newTemplates = templates;
  $("#new-template").innerHTML = templates.map((template, index) =>
    `<option value="${index}">${escapeHtml(template.label)}</option>`).join("");
  updateTemplateNote();
}

function updateTemplateNote() {
  const template = state.newTemplates[Number($("#new-template").value) || 0];
  $("#new-template-note").textContent = template?.note || "当前没有可用模板。";
  $("#create-draft").disabled = !template;
}

async function createDraft() {
  const template = state.newTemplates[Number($("#new-template").value)];
  if (!template) return;
  const id = Number($("#new-id").value);
  const position = Number($("#new-position").value);
  const name = $("#new-name").value.trim();
  if (!Number.isInteger(id) || id <= 0 || !Number.isInteger(position) || position <= 0 || !name) {
    showToast("请填写有效的新 ID、位置和名称");
    return;
  }
  if (state.bootstrap.machines[state.kind].some((machine) => machine.id === id)) {
    showToast(`Machine ID ${id} 已存在`);
    return;
  }
  try {
    const source = await apiGet(`/api/providers/capsules/api/machine?kind=${encodeURIComponent(state.kind)}&id=${template.machineId}`);
    const machine = deepClone(source.machine);
    machine.id = id;
    machine.name = name;
    machine.owner = "mod";
    machine.enabled = true;
    machine.retired = false;
    if (state.kind === "fixed_machines") {
      machine.currency = "GP";
      machine.draw_policy = "independent_weighted";
      machine.template_id = template.machineId;
      delete machine.grid_id;
      if (template.weights) {
        machine.slots.forEach((slot, index) => {
          slot.weight = template.weights[index];
        });
      }
    }
    state.selected = null;
    state.detail = {
      catalog_revision: state.revision,
      kind: state.kind,
      position,
      location: null,
      machine,
      isNew: true,
    };
    $("#new-dialog").close();
    renderMachineList();
    renderEditor();
    setDirty(true);
  } catch (error) {
    showToast(error.message);
  }
}

async function saveMachine(event) {
  event.preventDefault();
  showEditorError("");
  try {
    captureDraft();
    const detail = state.detail;
    const endpoint = detail.isNew ? "/api/providers/capsules/api/machines/create" : "/api/providers/capsules/api/machines/update";
    setSaveState("saving", "正在保存");
    const result = await apiPost(endpoint, {
      expected_revision: state.revision,
      kind: detail.kind,
      id: detail.machine.id,
      machine: detail.machine,
      position: detail.position,
    });
    const saved = result.machine;
    state.revision = result.catalog_revision;
    setDirty(false);
    await loadBootstrap({ kind: saved.kind, id: saved.id });
    const location = locationText(
      saved.kind,
      saved.native_location ?? saved.location
    );
    showResult("机台已保存", [
      `${kindLabel(saved.kind)} #${saved.id}`,
      `显示位置：${location}`,
      `revision：${result.catalog_revision}`,
      `备份：${result.backup_path}`,
    ]);
  } catch (error) {
    setSaveState("error", "保存失败");
    showEditorError(error.status === 409
      ? `${error.message} 请先保留当前编辑内容，再刷新页面确认其他进程的修改。`
      : error.message);
  }
}

async function retireOrRestore(action) {
  if (state.dirty && !window.confirm("当前修改尚未保存。继续会放弃这些修改，是否继续？")) return;
  const detail = state.detail;
  if (!detail || detail.isNew) return;
  const verb = action === "retire" ? "下架" : "恢复";
  if (action === "retire"
      && !window.confirm(`下架 #${detail.machine.id}？Machine ID 与历史记录会保留。`)) return;
  try {
    setSaveState("saving", `正在${verb}`);
    const result = await apiPost(`/api/providers/capsules/api/machines/${action}`, {
      expected_revision: state.revision,
      kind: detail.kind,
      id: detail.machine.id,
    });
    await loadBootstrap({ kind: detail.kind, id: detail.machine.id });
    showResult(`${verb}完成`, [
      `Machine ID：${detail.machine.id}`,
      `原生位置：${locationText(
        detail.kind,
        result.machine.native_location ?? result.machine.location
      )}`,
      `revision：${result.catalog_revision}`,
      `备份：${result.backup_path}`,
    ]);
  } catch (error) {
    setSaveState("error", `${verb}失败`);
    showEditorError(error.message);
  }
}

async function importCatalog(file) {
  if (!file) return;
  if (!confirmDiscard()) return;
  try {
    const catalog = JSON.parse(await file.text());
    const fixedCount = catalog.fixed_machines?.length ?? "?";
    const customCount = catalog.custom_machines?.length ?? "?";
    if (!window.confirm(
      `导入 ${file.name}？\n固定机 ${fixedCount} 台，自定义机 ${customCount} 台。\n现有目录会先生成内容寻址备份。`
    )) return;
    setSaveState("saving", "正在导入");
    const result = await apiPost("/api/providers/capsules/api/catalog/import", {
      expected_revision: state.revision,
      catalog,
    });
    clearEditor();
    await loadBootstrap();
    const moved = result.position_changes || [];
    const positionLines = moved.slice(0, 12).map((item) =>
      `${kindLabel(item.kind)} #${item.id}: ${item.old_position ?? "新增"} → ${locationText(
        item.kind,
        item.native_location ?? item.location
      )}`
    );
    showResult("配置已导入", [
      `固定机：${result.counts.fixed_machines} 台`,
      `自定义机：${result.counts.custom_machines} 台`,
      `revision：${result.catalog_revision}`,
      `备份：${result.backup_path}`,
      moved.length
        ? `新增或移动位置 ${moved.length} 项：\n${positionLines.join("\n")}${moved.length > 12 ? "\n…" : ""}`
        : "机台位置未发生变化。",
    ]);
  } catch (error) {
    setSaveState("error", "导入失败");
    showResult("导入失败", error.message);
  } finally {
    $("#import-file").value = "";
  }
}

async function exportBundle() {
  if (state.dirty) {
    showToast("请先保存当前修改，再导出配置包");
    return;
  }
  try {
    setSaveState("saving", "正在导出");
    const result = await apiPost("/api/providers/capsules/api/export", {
      expected_revision: state.revision,
      selected: state.selected,
    });
    setSaveState("clean", "已同步");
    const lines = [
      `目录：${result.bundle_path}`,
      `revision：${result.catalog_revision}`,
      `文件：${result.files.join("、")}`,
    ];
    if (result.selected_position) {
      lines.push(
        `当前机台：#${result.selected_position.id} · ${locationText(
          result.selected_position.kind,
          result.selected_position.native_location ?? result.selected_position.location
        )}`
      );
    }
    lines.push("客户端与服务端 JSON 使用同一个 catalog revision。");
    showResult("配置包已导出", lines);
  } catch (error) {
    setSaveState("error", "导出失败");
    showResult("导出失败", error.message);
  }
}

async function compileOverlay() {
  if (state.dirty) {
    showToast("请先保存当前修改，再编译客户端 Overlay");
    return;
  }
  try {
    setSaveState("saving", "正在编译 Overlay");
    const result = await apiPost("/api/providers/capsules/api/compile", {
      expected_revision: state.revision,
      selected: state.selected,
    });
    setSaveState("clean", "已同步");
    const counts = result.counts || {};
    const lines = [
      `输出：${result.output}`,
      `revision：${result.catalog_revision}`,
      `固定机：${counts.fixed ?? "?"} · Coin 变体：${counts.coin ?? "?"}`,
      `自定义机：${counts.custom ?? "?"} · 候选：${counts.candidates ?? "?"}`,
      result.reused
        ? "结果：已校验并复用相同 revision 的现有编译包。"
        : `结果：完成新编译${result.generated_cards == null ? "" : `，生成 ${result.generated_cards} 张卡片资源`}。`,
      `placements：${result.placements_path}`,
      "此动作只生成 Overlay 包，不会安装到游戏客户端。",
    ];
    if (result.selected_placement) {
      const placement = result.selected_placement;
      lines.splice(5, 0,
        `当前机台：#${placement.id} · 第 ${placement.position} 位 · 第 ${placement.page} 页第 ${placement.page_slot} 格 · grid ${placement.grid_id}`);
    }
    showResult("客户端 Overlay 已编译", lines);
  } catch (error) {
    setSaveState("error", "编译失败");
    showResult("客户端 Overlay 编译失败", error.message);
  }
}

function applyResultLines(result) {
  const lines = [
    result.message || (
      result.applied === true
        ? "客户端与服务端配置已按启动时预配置的目标导入。"
        : "部署器没有确认导入完成。"
    ),
    `revision：${result.catalog_revision}`,
    `运行时：${result.runtime_output}`,
    `placements：${result.placements_path}`,
  ];
  const optional = [
    ["客户端", result.client_target],
    ["服务端配置", result.server_ini],
    ["框架目录", result.framework_stage],
    ["回滚备份", result.backup_path || result.rollback_path],
  ];
  optional.forEach(([label, value]) => {
    if (value) lines.push(`${label}：${value}`);
  });
  if (Array.isArray(result.installed_files)) {
    lines.push(`已写入文件：${result.installed_files.length} 个`);
  }
  if (result.selected_placement) {
    const placement = result.selected_placement;
    lines.push(
      `当前机台：#${placement.id} · 第 ${placement.position} 位 · 第 ${placement.page} 页第 ${placement.page_slot} 格`
    );
  }
  return lines;
}

function runningClientMessage(message) {
  return /(running|in use|close(?: the)? client|正在运行|使用中|先关闭)/i.test(message);
}

async function applyToGame() {
  if (state.dirty) {
    showToast("请先保存当前修改，再导入到游戏");
    return;
  }
  if (!state.bootstrap?.capabilities.apply) {
    showToast("当前 UI 未预配置安装目标");
    return;
  }
  if (!window.confirm(
    "把当前 revision 导入启动 UI 时预配置的客户端与服务端？请先关闭目标游戏客户端。"
  )) return;
  try {
    setSaveState("saving", "正在导入到游戏");
    const result = await apiPost("/api/providers/capsules/api/apply", {
      expected_revision: state.revision,
      selected: state.selected,
    });
    if (result.applied !== true) {
      const message = result.message || result.error || "部署器没有返回 applied=true";
      setSaveState("error", "未导入");
      showResult(
        runningClientMessage(message) ? "请先关闭目标客户端" : "未完成导入",
        applyResultLines({ ...result, message })
      );
      return;
    }
    setSaveState("clean", "已导入游戏");
    showResult("已导入到游戏", applyResultLines(result));
  } catch (error) {
    setSaveState("error", "导入失败");
    showResult(
      runningClientMessage(error.message) ? "请先关闭目标客户端" : "导入失败",
      error.message
    );
  }
}

async function initializeRoutes() {
  if (state.dirty) {
    showToast("请先保存当前修改，再执行初始化");
    return;
  }
  const raw = window.prompt("新固定扭蛋机的 GP 价格：", "1200");
  if (raw === null) return;
  const price = Number(raw);
  if (!Number.isInteger(price) || price < 0) {
    showToast("价格必须是非负整数");
    return;
  }
  if (!window.confirm("根据已配置的只读快照与客户端补齐缺失取得路线？现有机台不会被删除。")) return;
  try {
    setSaveState("saving", "正在初始化");
    const result = await apiPost("/api/providers/capsules/api/initialize", {
      expected_revision: state.revision,
      price,
    });
    clearEditor();
    await loadBootstrap();
    const positions = result.new_machine_positions || [];
    showResult(result.report.remaining.length ? "初始化完成，仍有待处理项" : "初始化完成", [
      `新增固定机：${positions.length} 台`,
      ...positions.slice(0, 20).map((item) =>
        `#${item.id} · ${locationText(
          item.kind,
          item.native_location ?? item.location
        )}`),
      positions.length > 20 ? "…" : "",
      `新增自定义候选：${result.report.custom_candidate_additions.length} 项`,
      `本次补入获得入口：${result.report.seeds.length} 个`,
      `未自动配置的根：${(result.report.unconfigured_seeds || []).length} 个`,
      `仍待补足获得路线：${result.report.remaining.length} 台（详见报告中的原因）`,
      `报告：${result.report_path}`,
      `revision：${result.catalog_revision}`,
    ].filter(Boolean));
  } catch (error) {
    setSaveState("error", "初始化失败");
    showResult("初始化失败", error.message);
  }
}

function addCandidate() {
  try {
    captureDraft();
  } catch (error) {
    showEditorError(error.message);
    return;
  }
  const used = new Set(state.detail.machine.candidates.map((candidate) => candidate.unit_id));
  const unit = state.bootstrap.units.find((item) => item.eligible && item.can_add_custom !== false && !used.has(item.id))
    || state.bootstrap.units.find((item) => item.can_add_custom !== false && !used.has(item.id));
  if (!unit) {
    showToast("没有可添加的机体");
    return;
  }
  state.detail.machine.candidates.push({
    unit_id: unit.id,
    basic_weight: 1,
    mileage: 0,
    extra_price: 0,
    reset_mileage: false,
  });
  renderEditor();
  setDirty(true);
  openUnitPicker("candidate", state.detail.machine.candidates.length - 1);
}

function removeCandidate(index) {
  try {
    captureDraft();
  } catch (error) {
    showEditorError(error.message);
    return;
  }
  state.detail.machine.candidates.splice(index, 1);
  renderEditor();
  setDirty(true);
}

function bindEvents() {
  $$(".tab").forEach((tab) => {
    tab.addEventListener("click", () => {
      if (tab.dataset.kind === state.kind) return;
      if (!confirmDiscard()) return;
      state.kind = tab.dataset.kind;
      clearEditor();
      renderMachineList();
    });
  });
  $("#machine-search").addEventListener("input", renderMachineList);
  $$('input[name="machine-filter"]').forEach((radio) =>
    radio.addEventListener("change", renderMachineList));
  $("#new-machine").addEventListener("click", () => {
    if (confirmDiscard()) populateNewDialog();
  });
  $("#new-method").addEventListener("change", refreshNewTemplates);
  $("#new-template").addEventListener("change", updateTemplateNote);
  $("#create-draft").addEventListener("click", createDraft);
  $("#editor").addEventListener("submit", saveMachine);
  $("#editor").addEventListener("input", (event) => {
    if (event.target.classList.contains("slot-weight")) updateWeightTotal();
    setDirty(true);
  });
  $("#fixed-slots").addEventListener("click", (event) => {
    const button = event.target.closest("[data-pick-unit]");
    if (button) openUnitPicker("slot", Number(button.dataset.entryIndex));
  });
  $("#custom-candidates").addEventListener("click", (event) => {
    const picker = event.target.closest("[data-pick-unit]");
    if (picker) openUnitPicker("candidate", Number(picker.dataset.entryIndex));
    const remove = event.target.closest("[data-remove-candidate]");
    if (remove) removeCandidate(Number(remove.dataset.removeCandidate));
  });
  $("#add-candidate").addEventListener("click", addCandidate);
  $("#unit-search").addEventListener("input", renderUnitGrid);
  $("#retire-machine").addEventListener("click", () => retireOrRestore("retire"));
  $("#restore-machine").addEventListener("click", () => retireOrRestore("restore"));
  $("#import-button").addEventListener("click", () => $("#import-file").click());
  $("#import-file").addEventListener("change", (event) => importCatalog(event.target.files[0]));
  $("#export-button").addEventListener("click", exportBundle);
  $("#compile-button").addEventListener("click", compileOverlay);
  $("#apply-button").addEventListener("click", applyToGame);
  $("#initialize-button").addEventListener("click", initializeRoutes);
  $$("[data-close-dialog]").forEach((button) => {
    button.addEventListener("click", () => $(`#${button.dataset.closeDialog}`).close());
  });
  window.addEventListener("beforeunload", (event) => {
    if (!state.dirty) return;
    event.preventDefault();
    event.returnValue = "";
  });
}

async function init() {
  bindEvents();
  try {
    await loadBootstrap();
  } catch (error) {
    setSaveState("error", "读取失败");
    showResult("无法打开目录", error.message);
  }
}

await init();

}
