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
  portrait: null,
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
  const data = await apiGet("/api/bootstrap");
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
    const detail = await apiGet(`/api/machine?kind=${encodeURIComponent(kind)}&id=${id}`);
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
    <div class="unit-choice"><button class="unit-select-button" type="button"
            data-pick-unit="${targetType}" data-entry-index="${index}">
      <strong>${escapeHtml(name)}</strong>
      <small>${escapeHtml(subtitle)}</small>
    </button>${unit ? `<button class="portrait-open" type="button" data-edit-portrait="${unit.id}"
      ${Number(unit.rank) > 3 ? 'disabled title="此等级暂无已验证的原生卡框"' : ''}>头像 PNG / 取景</button>` : ''}</div>`;
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
    const source = await apiGet(`/api/machine?kind=${encodeURIComponent(state.kind)}&id=${template.machineId}`);
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
    const endpoint = detail.isNew ? "/api/machines/create" : "/api/machines/update";
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
    const result = await apiPost(`/api/machines/${action}`, {
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
    const result = await apiPost("/api/catalog/import", {
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
    const result = await apiPost("/api/export", {
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
    const result = await apiPost("/api/compile", {
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
    const result = await apiPost("/api/apply", {
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
  const raw = window.prompt("新固定机价格：留空按原版模板定价；填写整数则统一覆盖。", "");
  if (raw === null) return;
  const price = raw.trim() === "" ? null : Number(raw);
  if (price !== null && (!Number.isInteger(price) || price < 0)) {
    showToast("留空按模板定价，或填写非负整数");
    return;
  }
  if (!window.confirm("根据已配置的只读快照与客户端补齐缺失取得路线？现有机台不会被删除。")) return;
  try {
    setSaveState("saving", "正在初始化");
    const result = await apiPost("/api/initialize", {
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

function portraitError(message = "") {
  $("#portrait-error").textContent = message;
  $("#portrait-error").classList.toggle("hidden", !message);
}

function portraitChanged() {
  const p = state.portrait;
  if (!p) return;
  p.dirty = true;
  drawPortrait();
  clearTimeout(p.timer);
  p.timer = setTimeout(previewPortrait, 400);
}

function portraitFit(reset = false) {
  const p = state.portrait;
  if (!p?.image) return;
  const width = Math.min(p.image.width, p.image.height * 163 / 64);
  p.baseWidth = width;
  if (reset || !p.crop) {
    p.crop = [(p.image.width - width) / 2,
      (p.image.height - width * 64 / 163) / 2, width, width * 64 / 163];
  }
  $("#portrait-zoom").value = String(Math.max(100, Math.min(800, width / p.crop[2] * 100)));
  drawPortrait();
}

function clampPortraitCrop() {
  const p = state.portrait;
  if (!p?.image || !p.crop) return;
  p.crop[0] = Math.max(0, Math.min(p.image.width - p.crop[2], p.crop[0]));
  p.crop[1] = Math.max(0, Math.min(p.image.height - p.crop[3], p.crop[1]));
}

function drawPortrait() {
  const canvas = $("#portrait-canvas");
  const ctx = canvas.getContext("2d");
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  for (let y = 0; y < canvas.height; y += 16) {
    for (let x = 0; x < canvas.width; x += 16) {
      ctx.fillStyle = ((x + y) / 16) % 2 ? "#26323e" : "#1d2833";
      ctx.fillRect(x, y, 16, 16);
    }
  }
  const p = state.portrait;
  if (!p?.image || !p.crop) {
    ctx.fillStyle = "#b3c5d5";
    ctx.font = "18px sans-serif";
    ctx.textAlign = "center";
    ctx.fillText("上传 PNG，然后拖动取景框", canvas.width / 2, canvas.height / 2);
    return;
  }
  const scale = Math.min((canvas.width - 40) / p.image.width, (canvas.height - 40) / p.image.height);
  const ox = (canvas.width - p.image.width * scale) / 2;
  const oy = (canvas.height - p.image.height * scale) / 2;
  p.transform = { scale, ox, oy };
  ctx.drawImage(p.image, ox, oy, p.image.width * scale, p.image.height * scale);
  const [cx, cy, cw, ch] = p.crop;
  const x = ox + cx * scale, y = oy + cy * scale, w = cw * scale, h = ch * scale;
  ctx.fillStyle = "rgba(0, 0, 0, .56)";
  ctx.fillRect(0, 0, canvas.width, y);
  ctx.fillRect(0, y + h, canvas.width, canvas.height - y - h);
  ctx.fillRect(0, y, x, h);
  ctx.fillRect(x + w, y, canvas.width - x - w, h);
  ctx.strokeStyle = "#68e8ec";
  ctx.lineWidth = 2;
  ctx.strokeRect(x, y, w, h);
  ctx.fillStyle = "#efffff";
  for (const [px, py] of [[x,y],[x+w,y],[x,y+h],[x+w,y+h]]) ctx.fillRect(px-4, py-4, 8, 8);
  $("#portrait-source-info").textContent = `${p.image.width} × ${p.image.height} PNG · 取景 ${cw.toFixed(1)} × ${ch.toFixed(1)} · 拖动框，滚轮或滑杆缩放`;
  $("#portrait-zoom-value").textContent = `${(p.baseWidth / cw).toFixed(2)}×`;
}

async function setPortraitImage(dataUrl, crop = null) {
  const p = state.portrait;
  const image = new Image();
  await new Promise((resolve, reject) => {
    image.onload = resolve;
    image.onerror = () => reject(new Error("无法读取 PNG"));
    image.src = dataUrl;
  });
  if (state.portrait !== p) return;
  if (image.width > 2048 || image.height > 2048) throw new Error("PNG 最大为 2048 × 2048");
  p.image = image;
  p.png_base64 = dataUrl.split(",")[1];
  p.crop = crop ? [...crop] : null;
  portraitFit();
}

async function loadPortraitVariant() {
  const p = state.portrait;
  if (!p) return;
  const variant = Number($("#portrait-variant").value);
  p.variant = variant;
  p.image = null; p.crop = null; p.png_base64 = null; p.dirty = false;
  $("#portrait-file").value = "";
  clearTimeout(p.timer);
  p.previewSequence = (p.previewSequence || 0) + 1;
  portraitError(); drawPortrait();
  $("#portrait-source-info").textContent = "正在读取图片与取景…";
  $("#portrait-preview-status").textContent = "正在读取…";
  const result = await apiGet(`/api/portraits?unit_id=${p.unit.id}&variant=${variant}`);
  if (state.portrait !== p || p.variant !== variant) return;
  if (p.dirty) return;
  if (result.catalog_revision !== state.revision) throw new Error("配置已变化，请刷新页面后再编辑头像");
  if (result.image_data_url) await setPortraitImage(result.image_data_url, result.crop);
  const preview = $("#portrait-preview");
  preview.classList.toggle("hidden", !result.card_data_url);
  if (result.card_data_url) preview.src = result.card_data_url;
  $("#portrait-preview-status").textContent = result.overridden ? "已保存的头像与取景" : "当前原图；上传 PNG 后可调整取景";
}

async function openPortrait(unitId) {
  const unit = state.unitMap.get(unitId);
  if (!unit) return;
  state.portrait = { unit, variant: 0, dirty: false, previewSequence: 0 };
  $("#portrait-title").textContent = `${unit.name} · ${unit.grade} · 头像取景`;
  $("#portrait-variant").value = "0";
  $("#portrait-both").checked = false;
  $("#portrait-dialog").showModal();
  try { await loadPortraitVariant(); } catch (error) { portraitError(error.message); }
}

async function previewPortrait() {
  const p = state.portrait;
  if (!p?.png_base64 || !p.crop) return;
  const sequence = ++p.previewSequence;
  $("#portrait-preview-status").textContent = "正在合成原生卡片…";
  try {
    const result = await apiPost("/api/portraits/preview", {
      expected_revision: state.revision, unit_id: p.unit.id, variant: p.variant,
      png_base64: p.png_base64, crop: p.crop,
    });
    if (state.portrait !== p || sequence !== p.previewSequence) return;
    if (result.card_data_url) {
      $("#portrait-preview").src = result.card_data_url;
      $("#portrait-preview").classList.remove("hidden");
      $("#portrait-preview-status").textContent = "原生卡框、等级条和稀有剪影已合成；取景尚未保存";
    }
    portraitError();
  } catch (error) {
    if (state.portrait === p && sequence === p.previewSequence) {
      $("#portrait-preview-status").textContent = "预览不可用；可保存 PNG 与取景，配置客户端素材后编译。";
      portraitError(error.message);
    }
  }
}

async function savePortrait(reset = false) {
  const p = state.portrait;
  if (!p) return;
  if (!reset && (!p.png_base64 || !p.crop)) { portraitError("请先上传 PNG"); return; }
  const button = $(reset ? "#portrait-reset" : "#portrait-save");
  button.disabled = true;
  clearTimeout(p.timer);
  try {
    const payload = { expected_revision: state.revision, unit_id: p.unit.id, variant: p.variant };
    if (!reset) Object.assign(payload, { png_base64: p.png_base64, crop: p.crop, apply_to_both: $("#portrait-both").checked });
    const result = await apiPost(`/api/portraits/${reset ? 'reset' : 'save'}`, payload);
    state.revision = result.catalog_revision;
    state.bootstrap.catalog_revision = result.catalog_revision;
    $("#revision").textContent = `revision ${result.catalog_revision.slice(0,12)}`;
    p.dirty = false;
    await loadPortraitVariant();
    showToast(reset ? "已恢复当前姿态原图" : "头像与取景已保存，导入游戏后生效");
  } catch (error) { portraitError(error.message); }
  finally { button.disabled = false; }
}

function closePortrait() {
  const p = state.portrait;
  if (p?.dirty && !window.confirm("取景尚未保存，关闭并放弃本次修改？")) return;
  if (p) clearTimeout(p.timer);
  state.portrait = null;
  $("#portrait-dialog").close();
}

function bindPortraitEvents() {
  $("#portrait-close").addEventListener("click", closePortrait);
  $("#portrait-dialog").addEventListener("cancel", event => { event.preventDefault(); closePortrait(); });
  $("#portrait-file").addEventListener("change", async event => {
    const file = event.target.files[0]; if (!file) return;
    try {
      if (file.size > 4*1024*1024) throw new Error("PNG 最大为 4 MiB");
      if (file.type && file.type !== "image/png") throw new Error("只接受 PNG 图片");
      const dataUrl = await new Promise((resolve,reject) => {
        const reader = new FileReader(); reader.onload = () => resolve(reader.result);
        reader.onerror = reject; reader.readAsDataURL(file);
      });
      await setPortraitImage(dataUrl); portraitChanged();
    } catch (error) { portraitError(error.message || "PNG 读取失败"); }
  });
  $("#portrait-variant").addEventListener("change", async () => {
    const p = state.portrait;
    if (p?.dirty && !window.confirm("当前取景尚未保存，切换姿态并放弃修改？")) {
      $("#portrait-variant").value = String(p.variant); return;
    }
    try { await loadPortraitVariant(); } catch (error) { portraitError(error.message); }
  });
  $("#portrait-fit").addEventListener("click", () => { portraitFit(true); portraitChanged(); });
  const zoom = () => {
    const p = state.portrait; if (!p?.crop) return;
    const cx = p.crop[0]+p.crop[2]/2, cy = p.crop[1]+p.crop[3]/2;
    const w = p.baseWidth * 100 / Number($("#portrait-zoom").value), h = w*64/163;
    p.crop = [cx-w/2,cy-h/2,w,h]; clampPortraitCrop(); portraitChanged();
  };
  $("#portrait-zoom").addEventListener("input", zoom);
  const canvas = $("#portrait-canvas");
  canvas.addEventListener("wheel", event => {
    if (!state.portrait?.image) return; event.preventDefault();
    const slider = $("#portrait-zoom"); slider.value = String(Math.max(100,Math.min(800,Number(slider.value)+(event.deltaY<0?10:-10)))); zoom();
  }, {passive:false});
  const point = event => { const r=canvas.getBoundingClientRect(); return [(event.clientX-r.left)*canvas.width/r.width,(event.clientY-r.top)*canvas.height/r.height]; };
  canvas.addEventListener("pointerdown", event => {
    const p=state.portrait; if(!p?.image || !p.transform) return;
    const [x,y]=point(event), t=p.transform;
    const ix=(x-t.ox)/t.scale, iy=(y-t.oy)/t.scale;
    if(ix<0||iy<0||ix>p.image.width||iy>p.image.height) return;
    const corners = [[0,0],[1,0],[0,1],[1,1]];
    const corner = corners.find(([right,bottom]) => Math.hypot(
      x-(t.ox+(p.crop[0]+right*p.crop[2])*t.scale),
      y-(t.oy+(p.crop[1]+bottom*p.crop[3])*t.scale)) <= 12);
    if(!corner && (ix<p.crop[0]||ix>p.crop[0]+p.crop[2]||iy<p.crop[1]||iy>p.crop[1]+p.crop[3])) {
      p.crop[0]=ix-p.crop[2]/2; p.crop[1]=iy-p.crop[3]/2; clampPortraitCrop();
    }
    p.drag={x,y,crop:[...p.crop],corner}; canvas.setPointerCapture(event.pointerId); drawPortrait();
  });
  canvas.addEventListener("pointermove", event => {
    const p=state.portrait; if(!p?.drag) return;
    const [x,y]=point(event); p.crop=[...p.drag.crop];
    if (p.drag.corner) {
      const [right,bottom]=p.drag.corner, t=p.transform;
      const ax=p.drag.crop[0]+(1-right)*p.drag.crop[2];
      const ay=p.drag.crop[1]+(1-bottom)*p.drag.crop[3];
      const ix=(x-t.ox)/t.scale, iy=(y-t.oy)/t.scale;
      const maximum=Math.min(right?p.image.width-ax:ax,(bottom?p.image.height-ay:ay)*163/64,p.baseWidth);
      const width=Math.max(Math.min(p.baseWidth/8,maximum),Math.min(maximum,Math.max(Math.abs(ix-ax),Math.abs(iy-ay)*163/64)));
      const height=width*64/163;
      p.crop=[right?ax:ax-width,bottom?ay:ay-height,width,height];
      $("#portrait-zoom").value=String(Math.max(100,Math.min(800,p.baseWidth/width*100)));
    } else {
      p.crop[0]+=(x-p.drag.x)/p.transform.scale; p.crop[1]+=(y-p.drag.y)/p.transform.scale;
    }
    clampPortraitCrop(); portraitChanged();
  });
  const endDrag=()=>{const p=state.portrait;if(p?.drag){p.drag=null;portraitChanged();}};
  canvas.addEventListener("pointerup",endDrag); canvas.addEventListener("pointercancel",endDrag);
  $("#portrait-save").addEventListener("click",()=>savePortrait());
  $("#portrait-reset").addEventListener("click",()=>savePortrait(true));
  document.addEventListener("click",event=>{const b=event.target.closest("[data-edit-portrait]");if(b&&!b.disabled)openPortrait(Number(b.dataset.editPortrait));});
}

function bindEvents() {
  bindPortraitEvents();
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

init();
