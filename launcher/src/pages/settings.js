import { api, esc, icon, refreshIcons, toast, errText } from '../util.js';

/*
 * Settings schema. Key names come from nx/runtime/docs/CONFIG.md.
 *   file: "controls" (controls.ini), "pkp" (pkp.ini) or "config" (the launcher's own options)
 * Types: select, toggle, size, autonum, range, sens, path.
 */
const SECTIONS = [
  {
    title: 'Display', icon: 'monitor', file: 'controls',
    fields: [
      { id: 'window_mode', type: 'select', label: 'Window mode', sec: 'display', key: 'window_mode', def: 'windowed',
        options: [['windowed', 'Windowed'], ['borderless', 'Borderless fullscreen'], ['exclusive', 'Exclusive fullscreen']],
        hint: 'Set before starting; the picture size is fixed when the game starts.' },
      { id: 'size', type: 'size', label: 'Window size', sec: 'display', keys: ['width', 'height'], def: ['1280', '720'] },
      { id: 'render_scale', type: 'autonum', label: 'Render scale', sec: 'display', key: 'render_scale', def: 'auto',
        min: 25, max: 400, numDefault: 100, unit: '%', autoLabel: 'Auto', hint: 'Auto follows the window height. 100 is 1920x1080.' },
      { id: 'fps_limit', type: 'autonum', label: 'Frame rate limit', sec: 'display', key: 'fps_limit', def: 'auto',
        min: 0, max: 1000, numDefault: 60, unit: 'fps', autoLabel: 'Auto', hint: 'Auto follows the display refresh rate. 0 is unlimited.' },
      { id: 'vsync', type: 'toggle', label: 'VSync', sec: 'display', key: 'vsync', def: '1' },
    ],
  },
  {
    title: 'Audio', icon: 'volume-2', file: 'pkp',
    fields: [{ id: 'volume', type: 'range', label: 'Master volume', sec: 'audio', key: 'volume', def: '1.0' }],
  },
  {
    title: 'Mouse', icon: 'mouse', file: 'controls',
    fields: [
      { id: 'sens', type: 'sens', label: 'Sensitivity', sec: 'mouse', min: 0.1, max: 20, def: '2',
        hint: 'Source-engine scale, 2 is the default. Sets horizontal and vertical aim together.' },
      { id: 'invert_x', type: 'toggle', label: 'Invert horizontal', sec: 'mouse', key: 'invert_x', def: '0' },
      { id: 'invert_y', type: 'toggle', label: 'Invert vertical', sec: 'mouse', key: 'invert_y', def: '0' },
    ],
  },
  {
    title: 'Button prompts', icon: 'gamepad-2', file: 'controls',
    fields: [
      { id: 'prompt_style', type: 'select', label: 'Prompt style', sec: 'prompts', key: 'style', def: 'auto',
        options: [['auto', 'Automatic (follows the last device used)'], ['switch', 'Switch (the game\'s own)'], ['xbox', 'Xbox'], ['playstation', 'PlayStation'], ['keyboard', 'Keyboard and mouse']],
        hint: 'Applies on the next frame, with no restart.' },
    ],
  },
  {
    title: 'Paths', icon: 'folder', file: 'pkp',
    fields: [
      { id: 'romfs', type: 'path', label: 'RomFS folder', sec: 'fs', key: 'romfs', def: '', hint: 'Set from your game files. Change it here only to override.' },
      { id: 'save_dir', type: 'path', label: 'Save folder', sec: 'fs', key: 'save_dir', def: '', placeholder: 'save/ next to pkp.exe', hint: 'Empty: the launcher uses save/ next to pkp.exe.' },
      { id: 'shader_cache', type: 'path', label: 'Shader cache', sec: 'nvn', key: 'shader_cache', def: '', placeholder: 'shader_cache/spirv next to pkp.exe', hint: 'Folder with the translated shaders (.spv and results.tsv).' },
    ],
  },
  {
    title: 'Launcher', icon: 'wrench', file: 'config',
    fields: [
      { id: 'game_dir', type: 'path', label: 'Game folder', sec: '', key: 'game_dir', def: '', placeholder: 'the launcher\'s own folder', hint: 'The folder with pkp.exe, pkp.ini and controls.ini.' },
      { id: 'minimize_on_play', type: 'toggle', label: 'Minimise while the game runs', sec: '', key: 'minimize_on_play', def: '0' },
    ],
  },
];

const SOURCE_RAD = (0.022 * Math.PI) / 180; // radians per mouse count at Source sensitivity 1
const truthy = (v) => ['1', 'true', 'yes', 'on'].includes(String(v).trim().toLowerCase());
const clamp = (n, lo, hi) => Math.min(hi, Math.max(lo, n));
const fwd = (s) => String(s).trim().replace(/\\/g, '/');

export async function render(el, ctx) {
  const [data, cfg, status, ov] = await Promise.all([api('read_settings'), api('get_config'), api('game_status'), api('get_overview')]);
  if (ov.platform === 'linux') {
    // Linux: the AppImage is read-only, so the game has its own folder (default ~/Games/PokopiaRecomp)
    const gd = SECTIONS.flatMap((s) => s.fields).find((f) => f.id === 'game_dir');
    if (gd) gd.placeholder = ov.default_game_dir;
  }
  const files = { pkp: data.pkp, controls: data.controls, config: { 'game_dir': cfg.game_dir, 'minimize_on_play': cfg.minimize_on_play ? '1' : '0' } };
  const exists = { pkp: true, controls: data.controls_exists, config: true };
  const keyOf = (f, k) => (f.file === 'config' ? k : `${f.sec}.${k}`);
  const get = (f, k, def) => files[f.file][keyOf(f, k)] ?? def;

  // ----- model: one UI value per field -----
  const READ = {
    select: (f) => get(f, f.key, f.def),
    toggle: (f) => truthy(get(f, f.key, f.def)),
    size: (f) => ({ w: get(f, f.keys[0], f.def[0]), h: get(f, f.keys[1], f.def[1]) }),
    autonum: (f) => {
      const v = String(get(f, f.key, f.def));
      return v.toLowerCase() === 'auto' ? { auto: true, num: String(f.numDefault) } : { auto: false, num: v };
    },
    range: (f) => Math.round(clamp(parseFloat(get(f, f.key, f.def)) || 0, 0, 1) * 100),
    sens: (f) => {
      const rad = parseFloat(files.controls['mouse.yaw']);
      return String(Number.isFinite(rad) ? Math.round((rad / SOURCE_RAD) * 100) / 100 : f.def);
    },
    path: (f) => get(f, f.key, f.def),
  };
  const WRITE = {
    select: (f, x) => [[f.sec, f.key, x]],
    toggle: (f, x) => [[f.sec, f.key, x ? '1' : '0']],
    size: (f, x) => [
      [f.sec, f.keys[0], String(clamp(parseInt(x.w, 10) || 1280, 320, 16384))],
      [f.sec, f.keys[1], String(clamp(parseInt(x.h, 10) || 720, 320, 16384))],
    ],
    autonum: (f, x) => [[f.sec, f.key, x.auto ? 'auto' : String(clamp(Math.round(parseFloat(x.num)) || f.numDefault, f.min, f.max))]],
    range: (f, x) => [[f.sec, f.key, (x / 100).toFixed(2)]],
    sens: (f, x) => {
      const rad = (clamp(parseFloat(x) || 2, f.min, f.max) * SOURCE_RAD).toPrecision(9);
      return [[f.sec, 'yaw', rad], [f.sec, 'pitch', rad]];
    },
    path: (f, x) => [[f.sec, f.key, fwd(x)]],
  };

  const all = SECTIONS.flatMap((s) => s.fields.map((f) => ({ ...f, file: s.file })));
  const model = {};
  const orig = {};
  for (const f of all) {
    model[f.id] = READ[f.type](f);
    orig[f.id] = JSON.stringify(model[f.id]);
  }
  const dirtyFields = () => all.filter((f) => JSON.stringify(model[f.id]) !== orig[f.id]);

  // ----- markup -----
  const dis = (f) => (exists[f.file] ? '' : 'disabled');
  const CONTROL = {
    select: (f, x) => {
      const opts = f.options.some(([v]) => v === x) ? f.options : [...f.options, [x, x]];
      return `<select data-f="${f.id}" ${dis(f)}>${opts.map(([v, l]) => `<option value="${esc(v)}" ${v === x ? 'selected' : ''}>${esc(l)}</option>`).join('')}</select>`;
    },
    toggle: (f, x) => `<label class="switch"><input type="checkbox" data-f="${f.id}" ${x ? 'checked' : ''} ${dis(f)}>${x ? 'On' : 'Off'}</label>`,
    size: (f, x) => `<input type="number" data-f="${f.id}" data-part="w" min="320" max="16384" value="${esc(x.w)}" ${dis(f)}><span class="unit">x</span><input type="number" data-f="${f.id}" data-part="h" min="320" max="16384" value="${esc(x.h)}" ${dis(f)}>`,
    autonum: (f, x) => `<label class="switch"><input type="checkbox" data-f="${f.id}" data-part="auto" ${x.auto ? 'checked' : ''} ${dis(f)}>${esc(f.autoLabel)}</label><input type="number" data-f="${f.id}" data-part="num" min="${f.min}" max="${f.max}" value="${esc(x.num)}" ${x.auto || !exists[f.file] ? 'disabled' : ''}><span class="unit">${esc(f.unit)}</span>`,
    range: (f, x) => `<input type="range" data-f="${f.id}" min="0" max="100" step="1" value="${x}" ${dis(f)}><span class="unit" data-readout="${f.id}" style="min-width: 3.5ch; text-align: right">${x}%</span>`,
    sens: (f, x) => `<input type="number" data-f="${f.id}" min="${f.min}" max="${f.max}" step="0.1" value="${esc(x)}" ${dis(f)}>`,
    path: (f, x) => `<input type="text" data-f="${f.id}" spellcheck="false" placeholder="${esc(f.placeholder || '')}" value="${esc(x)}" ${dis(f)}><button class="btn" data-browse="${f.id}" ${dis(f)}>${icon('folder-open')}Browse</button>`,
  };

  const fieldHtml = (f) => `
    <div class="field">
      <label class="name">${esc(f.label)}${f.hint ? `<span class="hint">${esc(f.hint)}</span>` : ''}</label>
      <div class="control">${CONTROL[f.type](f, model[f.id])}</div>
    </div>`;

  function draw() {
    const n = dirtyFields().length;
    el.innerHTML = `
      <div class="page">
        <div class="hero">
          <h1>Settings</h1>
          <p class="dim">These edit pkp.ini and controls.ini next to pkp.exe. Other keys and comments in those files are left alone.</p>
        </div>
        ${status.state === 'running' ? `<div class="banner warn">${icon('triangle-alert')}<div>The game is running. Most changes apply at the next start, and the game may overwrite controls.ini when it exits.</div></div>` : ''}
        ${!data.controls_exists ? `<div class="banner info">${icon('info')}<div>controls.ini does not exist yet in <code>${esc(data.game_dir)}</code>. The game creates it on its first start, so Display, Mouse and Button prompts become editable after that. Audio, Paths and Launcher options work now.</div></div>` : ''}
        ${SECTIONS.map((s) => `<div class="card"><h2>${icon(s.icon)}${esc(s.title)}</h2>${s.fields.map((f) => fieldHtml({ ...f, file: s.file })).join('')}</div>`).join('')}
        <div class="savebar">
          <span class="dim">${n ? `${n} unsaved change${n > 1 ? 's' : ''}` : 'No changes'}</span>
          <div class="row">
            <button class="btn quiet" id="revert" ${n ? '' : 'disabled'}>${icon('undo-2')}Revert</button>
            <button class="btn primary" id="save" ${n ? '' : 'disabled'}>${icon('save')}Save</button>
          </div>
        </div>
      </div>`;
    refreshIcons();
    bind();
  }

  function bind() {
    const byId = Object.fromEntries(all.map((f) => [f.id, f]));
    el.querySelectorAll('[data-f]').forEach((input) => {
      const f = byId[input.dataset.f];
      const part = input.dataset.part;
      const onChange = () => {
        const v = input.type === 'checkbox' ? input.checked : input.value;
        if (part) model[f.id] = { ...model[f.id], [part]: v };
        else model[f.id] = f.type === 'range' ? parseInt(v, 10) : v;
        if (f.type === 'range') {
          el.querySelector(`[data-readout="${f.id}"]`).textContent = `${model[f.id]}%`;
          updateBar();
        } else if (input.type === 'text' || input.type === 'number') {
          updateBar();
        } else {
          draw(); // toggles and selects change dependent markup (labels, disabled inputs)
        }
      };
      input.addEventListener(input.type === 'range' || input.type === 'text' || input.type === 'number' ? 'input' : 'change', onChange);
    });
    el.querySelectorAll('[data-browse]').forEach((b) =>
      b.addEventListener('click', async () => {
        const f = byId[b.dataset.browse];
        const picked = await api('pick_folder', { title: `Choose: ${f.label}`, start: model[f.id] || null });
        if (picked) {
          model[f.id] = picked;
          draw();
        }
      }),
    );
    el.querySelector('#revert').addEventListener('click', () => {
      for (const f of all) model[f.id] = JSON.parse(orig[f.id]);
      draw();
    });
    el.querySelector('#save').addEventListener('click', save);
  }

  function updateBar() {
    const n = dirtyFields().length;
    const bar = el.querySelector('.savebar');
    bar.querySelector('span').textContent = n ? `${n} unsaved change${n > 1 ? 's' : ''}` : 'No changes';
    bar.querySelector('#save').disabled = !n;
    bar.querySelector('#revert').disabled = !n;
  }

  async function save() {
    const edits = [];
    const cfgNew = { ...cfg };
    for (const f of dirtyFields()) {
      for (const [sec, key, value] of WRITE[f.type](f, model[f.id])) {
        if (f.file === 'config') {
          cfgNew[key] = key === 'minimize_on_play' ? value === '1' : fwd(value);
        } else {
          edits.push({ file: f.file, section: sec, key, value });
        }
      }
    }
    try {
      if (edits.length) await api('write_settings', { edits });
      if (JSON.stringify(cfgNew) !== JSON.stringify(cfg)) await api('save_config', { config: cfgNew });
      toast('Settings saved.', 'ok');
      ctx.go('settings');
    } catch (e) {
      toast(errText(e), 'error');
    }
  }

  draw();
}
