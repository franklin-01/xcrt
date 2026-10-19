// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Franklin Cordeiro

// xcrt for Cinnamon: applies the xcrt CRT shader to the whole desktop.
//
// Cinnamon (muffin) is already the compositor, so instead of running the
// standalone xcrt compositor the shader is attached to the Clutter stage as a
// Cinnamon.GLSLEffect. The system cursor is replaced by a sprite inside the
// stage so it follows the curvature, just like in the standalone version.

const { Clutter, Cinnamon, Gio, GLib, GObject, Meta } = imports.gi;
const ByteArray = imports.byteArray;
const Main = imports.ui.main;
const Settings = imports.ui.settings;
const Magnifier = imports.ui.magnifier;

const MAX_MONITORS = 8;
const POINTER_POLL_MS = 8;

const PARAMS = [
    'curvature-x', 'curvature-y', 'corner-radius',
    'scanline-strength', 'scanline-size',
    'mask', 'mask-strength', 'mask-size',
    'softness', 'chroma', 'bloom', 'bloom-radius', 'vignette',
    'brightness', 'contrast', 'saturation', 'gamma',
    'monochrome', 'tint',
    'noise', 'flicker', 'jitter', 'roll', 'roll-speed',
];

const NEUTRAL = {
    'curvature-x': 0, 'curvature-y': 0, 'corner-radius': 0,
    'scanline-strength': 0, 'scanline-size': 3,
    'mask': 'none', 'mask-strength': 0, 'mask-size': 3,
    'softness': 0, 'chroma': 0, 'bloom': 0, 'bloom-radius': 1, 'vignette': 0,
    'brightness': 1, 'contrast': 1, 'saturation': 1, 'gamma': 1,
    'monochrome': false, 'tint': 'rgb(255,255,255)',
    'noise': 0, 'flicker': 0, 'jitter': 0, 'roll': 0, 'roll-speed': 0.1,
};

// Same presets as the standalone xcrt (without phosphor persistence).
const PRESETS = {
    arcade: {
        ...NEUTRAL,
        'curvature-x': 0.10, 'curvature-y': 0.10, 'corner-radius': 0.03,
        'scanline-strength': 0.55, 'scanline-size': 4,
        'mask': 'slot', 'mask-strength': 0.30, 'mask-size': 3,
        'softness': 0.6, 'chroma': 0.8, 'bloom': 0.35, 'vignette': 0.35,
        'noise': 0.02, 'flicker': 0.015, 'brightness': 1.25, 'saturation': 1.15,
    },
    pc: {
        ...NEUTRAL,
        'curvature-x': 0.025, 'curvature-y': 0.025, 'corner-radius': 0.012,
        'scanline-strength': 0.25, 'scanline-size': 3,
        'mask': 'aperture', 'mask-strength': 0.12, 'mask-size': 3,
        'softness': 0.25, 'chroma': 0.25, 'bloom': 0.10, 'vignette': 0.15,
        'brightness': 1.08,
    },
    trinitron: {
        ...NEUTRAL,
        'curvature-x': 0.05, 'curvature-y': 0.0, 'corner-radius': 0.005,
        'scanline-strength': 0.40, 'scanline-size': 3,
        'mask': 'aperture', 'mask-strength': 0.35, 'mask-size': 3,
        'softness': 0.35, 'chroma': 0.3, 'bloom': 0.25, 'vignette': 0.20,
        'brightness': 1.22, 'saturation': 1.10,
    },
    tv: {
        ...NEUTRAL,
        'curvature-x': 0.14, 'curvature-y': 0.14, 'corner-radius': 0.05,
        'scanline-strength': 0.50, 'scanline-size': 4,
        'mask': 'shadow', 'mask-strength': 0.30, 'mask-size': 4,
        'softness': 1.2, 'chroma': 1.6, 'bloom': 0.45, 'vignette': 0.50,
        'noise': 0.07, 'flicker': 0.04, 'jitter': 0.6, 'roll': 0.06, 'roll-speed': 0.08,
        'brightness': 1.30, 'saturation': 1.25, 'contrast': 1.05,
    },
    green: {
        ...NEUTRAL,
        'curvature-x': 0.07, 'curvature-y': 0.07, 'corner-radius': 0.03,
        'scanline-strength': 0.45, 'scanline-size': 3,
        'softness': 0.5, 'bloom': 0.55, 'bloom-radius': 1.3, 'vignette': 0.30,
        'noise': 0.025, 'flicker': 0.02,
        'monochrome': true, 'tint': 'rgb(51,255,102)', 'gamma': 1.1, 'brightness': 1.20,
    },
    amber: {
        ...NEUTRAL,
        'curvature-x': 0.07, 'curvature-y': 0.07, 'corner-radius': 0.03,
        'scanline-strength': 0.45, 'scanline-size': 3,
        'softness': 0.5, 'bloom': 0.55, 'bloom-radius': 1.3, 'vignette': 0.30,
        'noise': 0.025, 'flicker': 0.02,
        'monochrome': true, 'tint': 'rgb(255,176,0)', 'gamma': 1.1, 'brightness': 1.20,
    },
};
const PRESET_ORDER = ['arcade', 'pc', 'trinitron', 'tv', 'green', 'amber'];
const MASKS = { none: 0, aperture: 1, slot: 2, shadow: 3 };

let shaderSource = null;
let CrtEffect = null;
let xcrt = null;

function defineEffect() {
    // A unique type name lets the extension be reloaded without restarting Cinnamon.
    return GObject.registerClass({
        GTypeName: `XcrtCrtEffect${Date.now()}`,
    }, class extends Cinnamon.GLSLEffect {
        _init(params) {
            super._init(params);
            this._locations = {};
        }

        vfunc_build_pipeline() {
            this.add_glsl_snippet(Cinnamon.SnippetHook.TEXTURE_LOOKUP, shaderSource,
                'cogl_texel = xcrt_crt(cogl_sampler, cogl_tex_coord.st);', true);
        }

        setUniform(name, components, values) {
            let loc = this._locations[name];
            if (loc === undefined)
                loc = this._locations[name] = this.get_uniform_location(name);
            if (loc >= 0)
                this.set_uniform_float(loc, components, values);
        }
    });
}

function nearlyEqual(a, b) {
    if (typeof a === 'number' && typeof b === 'number')
        return Math.abs(a - b) < 1e-4;
    return a === b;
}

function parseColor(str) {
    let [ok, c] = Clutter.Color.from_string(str || 'white');
    return ok ? [c.red / 255, c.green / 255, c.blue / 255] : [1, 1, 1];
}

class Xcrt {
    constructor(meta) {
        this._settings = new Settings.ExtensionSettings(this, meta.uuid);
        for (const key of PARAMS)
            this._settings.bind(key, `_${key}`, () => this._onParamChanged());
        this._settings.bind('curvature-scale', '_curvatureScale', () => {
            this._updateUniforms();
            this._saveBackup();
        });
        this._settings.bind('preset', '_preset', () => this._onPresetChanged());
        this._settings.bind('active', '_active', () => this._sync());
        this._settings.bind('draw-cursor', '_drawCursor', () => this._sync(true));
        this._settings.bind('fullscreen', '_fullscreen', () => this._sync(true));
        for (const key of ['toggle-key', 'next-preset-key', 'curvature-up-key', 'curvature-down-key'])
            this._settings.bind(key, `_${key}`, () => this._bindKeys());

        this._effect = null;
        this._cursor = null;
        this._monitorsId = Main.layoutManager.connect('monitors-changed', () => this._updateUniforms());
        this._t0 = GLib.get_monotonic_time();
        this._frame = 0;
        this._repairUpgrade();
        this._saveBackup();
        this._bindKeys();
        this._sync();
    }

    get(key) {
        return this._settings.getValue(key);
    }

    destroy() {
        this._stop();
        this._unbindKeys();
        Main.layoutManager.disconnect(this._monitorsId);
        this._settings.finalize();
    }

    // ---- lifecycle -----------------------------------------------------

    _sync(restart = false) {
        const active = this.get('active');
        if (this._effect && (!active || restart))
            this._stop();
        if (active && !this._effect)
            this._start();
    }

    _start() {
        this._effect = new CrtEffect();
        global.stage.add_effect_with_name('xcrt', this._effect);
        // Clutter redraws only the damaged region, measured before the warp; with
        // curvature any change moves pixels elsewhere, so every redraw must be full.
        this._redrawId = global.stage.connect('queue-redraw', (stage, origin) => {
            if (origin !== stage && !this._fullRedrawQueued) {
                this._fullRedrawQueued = true;
                stage.queue_redraw();
                this._fullRedrawQueued = false;
            }
        });
        if (this.get('fullscreen')) {
            // fullscreen games and videos bypass the stage unless unredirection is off
            Meta.disable_unredirect_for_display(global.display);
            this._unredirectDisabled = true;
        }
        if (this.get('draw-cursor'))
            this._startCursor();
        this._updateUniforms();
    }

    _stop() {
        this._stopAnimation();
        this._stopCursor();
        if (this._unredirectDisabled) {
            Meta.enable_unredirect_for_display(global.display);
            this._unredirectDisabled = false;
        }
        if (this._redrawId) {
            global.stage.disconnect(this._redrawId);
            this._redrawId = 0;
        }
        if (this._effect) {
            global.stage.remove_effect(this._effect);
            this._effect = null;
        }
    }

    // ---- software cursor -------------------------------------------------

    _startCursor() {
        this._tracker = Meta.CursorTracker.get_for_display(global.display);
        this._cursor = new Clutter.Actor({ request_mode: Clutter.RequestMode.CONTENT_SIZE, reactive: false });
        this._cursor.content = new Magnifier.MouseSpriteContent();
        global.stage.add_child(this._cursor);
        this._hot = [0, 0];
        this._pointer = [-1, -1];
        this._cursorChangedId = this._tracker.connect('cursor-changed', () => this._updateSprite());
        // muffin shows the system pointer again whenever the input device changes
        // (moving the mouse after typing, for example), so hide it every time
        this._visibilityId = this._tracker.connect('visibility-changed', () => {
            if (this._tracker.get_pointer_visible())
                this._tracker.set_pointer_visible(false);
        });
        this._updateSprite();
        this._tracker.set_pointer_visible(false);
        // the X11 backend has no pointer-motion signal for the whole screen
        this._pollId = GLib.timeout_add(GLib.PRIORITY_HIGH, POINTER_POLL_MS, () => {
            this._updatePointer(false);
            return GLib.SOURCE_CONTINUE;
        });
    }

    _stopCursor() {
        if (!this._cursor)
            return;
        GLib.source_remove(this._pollId);
        this._tracker.disconnect(this._cursorChangedId);
        this._tracker.disconnect(this._visibilityId);
        this._tracker.set_pointer_visible(true);
        this._cursor.destroy();
        this._cursor = null;
    }

    _updateSprite() {
        this._cursor.content.texture = this._tracker.get_sprite();
        this._hot = this._tracker.get_hot();
        this._updatePointer(true);
    }

    _updatePointer(force) {
        const [x, y] = global.get_pointer();
        if (!force && x === this._pointer[0] && y === this._pointer[1])
            return;
        this._pointer = [x, y];
        this._cursor.set_position(x - this._hot[0], y - this._hot[1]);
        global.stage.set_child_above_sibling(this._cursor, null);
    }

    // ---- animation (noise, flicker, jitter, rolling bar) ---------------

    _startAnimation() {
        if (this._timeline)
            return;
        this._timeline = new Clutter.Timeline({ duration: 1000, repeat_count: -1 });
        this._timeline.connect('new-frame', () => this._animate());
        this._timeline.start();
    }

    _stopAnimation() {
        if (!this._timeline)
            return;
        this._timeline.stop();
        this._timeline = null;
    }

    _animate() {
        if (!this._effect)
            return;
        const t = ((GLib.get_monotonic_time() - this._t0) / 1e6) % 3600;
        this._effect.setUniform('xcrt_time', 1, [t]);
        this._effect.setUniform('xcrt_frame', 1, [this._frame++ % 100000]);
        this._effect.queue_repaint();
    }

    // ---- uniforms ------------------------------------------------------

    _updateUniforms() {
        const e = this._effect;
        if (!e)
            return;
        const g = (key) => this.get(key);
        const k = g('curvature-scale') / 100;

        const mons = Main.layoutManager.monitors.slice(0, MAX_MONITORS);
        const rects = [];
        for (const m of mons)
            rects.push(m.x, m.y, m.width, m.height);
        e.setUniform('xcrt_stage', 2, [global.screen_width, global.screen_height]);
        e.setUniform('xcrt_mons', 4, rects.length ? rects : [0, 0, global.screen_width, global.screen_height]);
        e.setUniform('xcrt_nmons', 1, [Math.max(mons.length, 1)]);

        e.setUniform('xcrt_curvature', 2, [g('curvature-x') * k, g('curvature-y') * k]);
        e.setUniform('xcrt_corner', 1, [g('corner-radius')]);
        e.setUniform('xcrt_scanline', 2, [g('scanline-strength'), g('scanline-size')]);
        e.setUniform('xcrt_mask', 3, [MASKS[g('mask')] || 0, g('mask-strength'), g('mask-size')]);
        e.setUniform('xcrt_beam', 2, [g('softness'), g('chroma')]);
        e.setUniform('xcrt_bloom', 2, [g('bloom'), g('bloom-radius')]);
        e.setUniform('xcrt_vignette', 1, [g('vignette')]);
        e.setUniform('xcrt_noise', 3, [g('noise'), g('flicker'), g('jitter')]);
        e.setUniform('xcrt_roll', 2, [g('roll'), g('roll-speed')]);
        e.setUniform('xcrt_grade', 4, [g('brightness'), g('contrast'), g('saturation'), g('gamma')]);
        e.setUniform('xcrt_tint', 4, [...parseColor(g('tint')), g('monochrome') ? 1 : 0]);

        const animated = g('noise') > 0 || g('flicker') > 0 || g('jitter') > 0 || g('roll') > 0;
        if (animated)
            this._startAnimation();
        else
            this._stopAnimation();
        this._animate();
    }

    // ---- surviving settings upgrades -----------------------------------

    // Cinnamon's settings upgrade (run whenever settings-schema.json changes)
    // treats a slider value sitting exactly on its min or max as invalid and
    // resets it to the default. Undo exactly that from the last known values.
    _repairUpgrade() {
        const backup = this.get('param-backup') || {};
        const data = this._settings.settingsData || {};
        for (const key of [...PARAMS, 'curvature-scale']) {
            const info = data[key];
            if (!(key in backup) || !info || info.type !== 'scale')
                continue;
            const saved = backup[key];
            const onEdge = nearlyEqual(saved, info.min) || nearlyEqual(saved, info.max);
            if (onEdge && nearlyEqual(this.get(key), info.default) && !nearlyEqual(saved, info.default)) {
                this._settings.setValue(key, saved);
                global.log(`xcrt: restored ${key} = ${saved} after a settings upgrade`);
            }
        }
    }

    _saveBackup() {
        const backup = {};
        for (const key of [...PARAMS, 'curvature-scale'])
            backup[key] = this.get(key);
        this._settings.setValue('param-backup', backup);
    }

    // ---- presets -------------------------------------------------------

    _matchesPreset(name) {
        const p = PRESETS[name];
        return p && PARAMS.every((key) => nearlyEqual(this.get(key), p[key]));
    }

    _applyPreset(name) {
        const p = PRESETS[name];
        if (!p)
            return;
        for (const key of PARAMS)
            if (!nearlyEqual(this.get(key), p[key]))
                this._settings.setValue(key, p[key]);
        this._updateUniforms();
        this._saveBackup();
    }

    _onPresetChanged() {
        const name = this.get('preset');
        if (name !== 'custom' && !this._matchesPreset(name))
            this._applyPreset(name);
    }

    _onParamChanged() {
        this._updateUniforms();
        this._saveBackup();
        // editing an effect by hand turns the preset into "custom"
        const name = this.get('preset');
        if (name !== 'custom' && !this._matchesPreset(name))
            this._settings.setValue('preset', 'custom');
    }

    // ---- hotkeys -------------------------------------------------------

    _bindKeys() {
        this._unbindKeys();
        const keys = {
            'toggle-key': () => this._toggle(),
            'next-preset-key': () => this._nextPreset(),
            'curvature-up-key': () => this._stepCurvature(10),
            'curvature-down-key': () => this._stepCurvature(-10),
        };
        this._hotkeys = [];
        for (const [key, cb] of Object.entries(keys)) {
            const binding = this.get(key);
            if (!binding)
                continue;
            const name = `xcrt-${key}`;
            Main.keybindingManager.addHotKey(name, binding, cb);
            this._hotkeys.push(name);
        }
    }

    _unbindKeys() {
        for (const name of this._hotkeys || [])
            Main.keybindingManager.removeHotKey(name);
        this._hotkeys = [];
    }

    _osd(text, level = null) {
        const icon = Gio.ThemedIcon.new('video-display-symbolic');
        Main.osdWindowManager.show(-1, icon, text, level);
    }

    _toggle() {
        this._settings.setValue('active', !this.get('active'));
        this._sync();
        this._osd(this.get('active') ? 'CRT filter on' : 'CRT filter off');
    }

    _nextPreset() {
        const cur = PRESET_ORDER.indexOf(this.get('preset'));
        const name = PRESET_ORDER[(cur + 1) % PRESET_ORDER.length];
        this._settings.setValue('preset', name);
        this._applyPreset(name);
        this._osd(`Preset: ${name}`);
    }

    _stepCurvature(delta) {
        const v = Math.min(300, Math.max(0, Math.round(this.get('curvature-scale') + delta)));
        this._settings.setValue('curvature-scale', v);
        this._updateUniforms();
        this._saveBackup();
        this._osd(`Curvature ${v}%`, Math.round(v / 3));
    }
}

let extensionMeta = null;

function init(metadata) {
    extensionMeta = metadata;
    const [, bytes] = GLib.file_get_contents(`${metadata.path}/crt.glsl`);
    shaderSource = ByteArray.toString(bytes);
    CrtEffect = defineEffect();
}

function enable() {
    xcrt = new Xcrt(extensionMeta);
}

function disable() {
    if (xcrt) {
        xcrt.destroy();
        xcrt = null;
    }
}
