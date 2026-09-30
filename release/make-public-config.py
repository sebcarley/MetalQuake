#!/usr/bin/env python3
"""release/make-public-config.py <his config.cfg> <out.cfg>

The PUBLIC build's default settings, from an ALLOW-LIST (REVIEW.md 0.9): the Best tier
and the approved LOOK, and nothing else. A cvar ships because it is NAMED here --
RELEASE.md's own allow-list rule, which the deny-list this replaced broke: it shipped
Seb's gameplay presets, his machine's leftovers and the eleven BEAUTY extras switched off.

  tier     every lever of menu.c's m5_quality_levers[] at the Best row, read from the
           table itself, so a retier cannot leave this file behind
  look     LOOK below, at the value in Seb's config, so a look he re-tunes reaches the
           next build by itself; a LOOK cvar absent from his config is at its default
           there, and ships at the default
  forced   FORCE below
Everything else ships at the ENGINE'S default: the gameplay extras (the fun mods and the
gore preset's own outputs), binds, video mode, mouse, sound levels, names, records and
developer chatter -- and the eleven BEAUTY extras, which every tier keeps ON (menu.c, the
2026-09-19 retier) and which his rich.cfg switched off. Every cvar in his config that does
not ship is printed with the reason, so a newly tuned look cvar is seen rather than lost:
add it to LOOK if it belongs to the picture.
"""
import glob, importlib.util, os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.join(HERE, '..')
spec = importlib.util.spec_from_file_location('ct', os.path.join(ROOT, 'test', 'perf', 'check-tiers.py'))
ct = importlib.util.module_from_spec(spec); spec.loader.exec_module(ct)

LOOK = (
    # the murk: colour, density, reach, the ground layer, the noise, the water
    'r_volumetric_color_red', 'r_volumetric_color_green', 'r_volumetric_color_blue',
    'r_volumetric_corner', 'r_volumetric_density', 'r_volumetric_dist',
    'r_volumetric_ground', 'r_volumetric_groundheight', 'r_volumetric_groundthresh',
    'r_volumetric_groundcolor_red', 'r_volumetric_groundcolor_green', 'r_volumetric_groundcolor_blue',
    'r_volumetric_lavaglow', 'r_volumetric_noisescale', 'r_volumetric_noise2_contrast',
    'r_volumetric_noise2_ridge', 'r_volumetric_noise2_warp', 'r_volumetric_swirl',
    'r_volumetric_watercolor_red', 'r_volumetric_watercolor_green', 'r_volumetric_watercolor_blue',
    'r_volumetric_waterdensity', 'r_volumetric_watermist',
    # the ray-traced light
    'rt_metal_walllight', 'rt_metal_ambient', 'rt_metal_color', 'rt_metal_darkness',
    'rt_metal_softness', 'rt_metal_history', 'rt_metal_viewmodel', 'rt_metal_liquids',
    'rt_metal_liquids_rt', 'rt_metal_liquids_reflect', 'rt_metal_liquids_ripple',   # 2026-09-30: his water (VKRT 1/1b)
    'rt_metal_lavalights', 'rt_metal_jitter',
    # water
    'r_wateralpha', 'r_wateralpha_force',
    # the frame and its tone: HDR, the highlight shoulder, glow, bloom, and the brightness
    # curve the look was approved under -- tuned on an OLED; on an SDR monitor it caps
    # low (REVIEW.md 3.3), and Options -> Brightness and Gamma -> Reset is the neutral curve
    'r_viewfbo', 'r_edr', 'r_gamma_analytic', 'r_hdr_displayfit', 'r_dither', 'r_hud_brightness',
    'r_hdr_shoulder', 'r_hdr_glowintensity',
    'r_hdr_scenebrightness', 'r_brightness', 'v_gamma', 'v_contrast',
    'r_bloom', 'r_bloom_m5', 'r_redglow', 'r_coronas', 'r_lerpsprites',
    # lava
    'r_lavaboil', 'r_lavashimmer', 'r_lavashimmer_path', 'r_lavashimmer_scale', 'r_lavashimmer_speed',
    # the thunderbolt as it was passed (cl_beams_* put the beam on the crosshair)
    'cl_beams_instantaimhack', 'cl_beams_quakepositionhack',
    'r_lightningbeam_color_red', 'r_lightningbeam_color_green', 'r_lightningbeam_color_blue',
    'r_lightningbeam_thickness', 'r_lightningbeam_scroll', 'r_lightningbeam_repeatdistance',
    'r_lightningbeam_m5_branches', 'r_lightningbeam_m5_corevolume', 'r_lightningbeam_m5_filaments',
    'r_lightningbeam_m5_fog', 'r_lightningbeam_m5_hitboost', 'r_lightningbeam_m5_hold',
    'r_lightningbeam_m5_impact', 'r_lightningbeam_m5_jitter', 'r_lightningbeam_m5_light',
    'r_lightningbeam_m5_rate', 'r_lightningbeam_m5_whiteness',
)
FORCE = {'vid_vsync': '1', 'scr_screenshot_jpeg': '0', 'scr_screenshot_png': '1'}   # PNG: libpng is bundled, libjpeg is not

# WHY the rest does not ship -- for the report, and for the guard below; what ships is
# decided above. Every entry is a PREFIX unless it ends in '$' (an exact name), and the
# first group that matches names the reason.
NOT_SHIPPED = (
    ('BEAUTY extra (ships at the engine default, which is ON)', (
        'cl_particles_texsize', 'cl_particles_blood_droplet', 'cl_particles_soft',
        'cl_particles_refract', 'cl_particles_scorchglow', 'rt_metal_gi_ao',
        'rt_metal_fog_liquidlight', 'rt_metal_contact', 'm5_torch_embers',
        'r_skylightning', 'r_caustics')),
    ('gameplay (ships at the engine default)', (
        'm5_movement', 'm5_gore', 'm5_burn', 'm5_venom', 'm5_shotgun', 'm5_bullettime',
        'm5_horde', 'm5_balllightning', 'm5_kick', 'm5_grenadebounce', 'm5_nailtracer',
        'm5_nailbarrels', 'm5_axesparks', 'm5_explosion_sprite', 'm5_powerupglow',
        # m5_torch EXACTLY (the '$'): the handlamp's switch is gameplay, but its
        # _softness/_fogweight/_filament knobs are look and must surface if tuned
        'm5_noclipfly', 'm5_torch$', 'm5_muzzleflash', 'm5_shotgun_casing', 'm5_venom_trail',
        'cl_stainmaps',
        # the m5_gore preset's own outputs (cl_main.c M5_Gore_c): shipped beside gore 0
        # they would rebuild "ludicrous" with its switch off
        'cl_particles_quality', 'cl_particles_blood_decal_scale', 'cl_decals_time',
        'cl_decals_fadetime')),
    ('inert, overridden or the engine\'s own', (
        'r_bloom_blur', 'r_bloom_brighten', 'r_bloom_colorexponent', 'r_bloom_colorscale',
        'r_bloom_colorsubtract', 'r_bloom_resolution',    # the 2001 chain; r_bloom_m5 ignores them
        'r_fxaa_post_span', 'rt_metal_shafts_history',    # r_fxaa_post and rt_metal_shafts are 0 on every tier
        'gl_texturecompression',                          # no Metal path at all
        'v_color_',                                       # inert at v_color_enable 0
        'rt_metal_sun_',                                  # would override every map's own sun keys
        'm5_packlight', 'm5_packbrightness',              # the per-pack look files own these
        'r_buffermegs_', 'r_framedatasize')),             # high-water marks the engine grows itself
    ('personal or this machine', (
        'm5_horde_best', 'developer', 'sensitivity', 'fov', 'crosshair', 'volume', 'bgmvolume',
        'snd_width', 'con_notify', 'con_chatsound', 'cl_showfps', 'viewsize', 'cl_startdemos',
        'vid_', '_cl_', 'cl_name', 'm_', 'joy', 'in_', 'net_', 'sv_', 'snd_speed',
        'snd_channels', 'scr_conalpha', 'sbar_', 'menu_', 'saved', 'cl_forwardspeed',
        'cl_backspeed', 'cl_sidespeed', 'lookspring', 'lookstrafe', 'freelook')),
)
UNLISTED = 'NOT ON THE ALLOW-LIST (add it to LOOK if it belongs to the picture)'

def why(name):
    for reason, prefixes in NOT_SHIPPED:
        for p in prefixes:
            if (name == p[:-1]) if p.endswith('$') else name.startswith(p):
                return reason
    return UNLISTED

def fmt(v):
    return ('%g' % v)

def main():
    src, dst = sys.argv[1], sys.argv[2]
    table = ct.parse_menu()
    best = ct.TIERS.index('Best')
    engine = ''.join(open(f, encoding='utf-8', errors='replace').read()
                     for f in glob.glob(os.path.join(ROOT, '*.c')) + glob.glob(os.path.join(ROOT, '*.m')))
    bad = ['%s is a tier lever (it comes from the table)' % n for n in LOOK if n in table]
    bad += ['%s is listed as look and as %s' % (n, why(n)) for n in LOOK if why(n) != UNLISTED]
    bad += ['%s is not a cvar the engine registers (a typo would never ship)' % n
            for n in LOOK if '"%s"' % n not in engine]
    if bad:
        print('make-public-config: FAIL -- ' + '; '.join(bad)); sys.exit(1)
    out, looked, dropped = [], 0, {}
    for line in open(src, encoding='utf-8', errors='replace'):
        m = re.match(r'^"([^"]+)"\s+"(.*)"\s*$', line)
        if not m:
            continue                        # binds, unbindall, comments
        name = m.group(1)
        if name in table or name in FORCE:
            continue                        # written below, from the table and FORCE
        if name in LOOK:
            out.append('"%s" "%s"' % (name, m.group(2))); looked += 1
        else:
            dropped.setdefault(why(name), []).append(name)
    for name, vals in sorted(table.items()):
        out.append('"%s" "%s"' % (name, fmt(vals[best])))
    for name, v in FORCE.items():
        out.append('"%s" "%s"' % (name, v))
    with open(dst, 'w') as f:
        f.write('// MetalQuake: the shipped default settings -- the Best tier and the reference\n'
                '// look, and nothing else: gameplay, keys, screen and sound are the engine\'s own\n'
                '// defaults. Generated by release/make-public-config.py from an allow-list. Your\n'
                '// own settings are saved elsewhere the first time you quit, and win from then\n'
                '// on; this file is never written to.\n')
        f.write('\n'.join(sorted(out)) + '\n')
    print('public config: %d cvars (%d tier levers at Best, %d look, %d forced) -> %s'
          % (len(out), len(table), looked, len(FORCE), dst))
    for reason, names in dropped.items():
        print('  not shipped, %s: %s' % (reason, ' '.join(sorted(names))))
    # An allow-list's failure mode is SILENCE: a look cvar tuned after this list was
    # written is simply left out of the next public build. So an unlisted name is FATAL
    # unless the release is told otherwise (RELEASE_ALLOW_UNLISTED=1, the
    # RELEASE_ALLOW_CUSTOM shape): classify it -- LOOK, or a NOT_SHIPPED group -- first.
    if dropped.get(UNLISTED) and os.environ.get('RELEASE_ALLOW_UNLISTED') != '1':
        print('make-public-config: FAIL -- %d cvar(s) in the source config are on neither list: %s'
              % (len(dropped[UNLISTED]), ' '.join(sorted(dropped[UNLISTED]))))
        print('  add each to LOOK or to a NOT_SHIPPED group, or set RELEASE_ALLOW_UNLISTED=1')
        sys.exit(1)

main()
