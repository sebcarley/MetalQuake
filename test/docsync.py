#!/usr/bin/env python3
"""docsync -- the documents' checkable claims, checked (and where possible, generated).

WHY THIS EXISTS.  This tree's documents carry a large transcription surface:
SETTINGS.md alone quotes ~190 cvar defaults by hand, CLAUDE.md quotes counts of
test cases and parity vantages, and three files carry the M5 Quality tier table.
Every one of those is a fact about the SOURCE, copied into prose, and every one
of them rots silently.  It has rotted repeatedly: the 2026-08-18 audit corrected
"16 XCTest cases" to 46 and "thirty-five vantages" to 36, and by 2026-08-30 the
XCTest figure had drifted again (46 against 58) while `rt_metal_gi_albedo_tex`'s
Default column still read 0 after the QA pass shipped it at 1.  A note WARNING
about staleness went stale in four of its nine values, including the single
claim it was written to protect.

So the fix is not another correction.  Anything derivable from the source is
either GENERATED here (between sentinels) or CHECKED here, and the check is a
smoke check, so the rot fails the build instead of misleading the next reader.

  python3 test/docsync.py              # check; exit 1 on any mismatch
  python3 test/docsync.py --refresh    # rewrite the generated blocks in place
  python3 test/docsync.py --refresh --config <config.cfg>
                                       # also regenerate the live-config snapshot

WHAT IT CANNOT DO.  Prose is not checkable in general -- "on for Best and Better"
is a claim about the tier table that no parser will reliably find.  What it CAN
do is check every claim that has a machine-readable shape, and the tier-count
word check below is a cheap net for the commonest prose rot (a four-tier phrase
surviving into a six-tier table).
"""
import glob, io, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
def P(*a): return os.path.join(ROOT, *a)

# Commands, not cvars: they appear in the same tables and have no default.
NOT_CVARS = {'r_edr_probe', 'r_edr_report', 'r_lightningbeam_m5_test',
             'r_volumetric_probe', 'r_speeds_dump', 'photomode'}

failures = []
notes = []
def fail(where, msg): failures.append('%s: %s' % (where, msg))
def note(msg): notes.append(msg)


# ---------------------------------------------------------------- the sources

def _split_fields(body):
    """Split a cvar_t initialiser on top-level commas, respecting quotes."""
    out, cur, depth, q, esc = [], [], 0, False, False
    for ch in body:
        if esc: cur.append(ch); esc = False; continue
        if ch == '\\' and q: cur.append(ch); esc = True; continue
        if ch == '"': q = not q; cur.append(ch); continue
        if not q:
            if ch in '([{': depth += 1
            elif ch in ')]}': depth -= 1
            elif ch == ',' and depth == 0:
                out.append(''.join(cur).strip()); cur = []; continue
        cur.append(ch)
    if cur: out.append(''.join(cur).strip())
    return out


def cvar_defaults():
    """Every cvar_t declaration's registered default, by name.

    The shape is `cvar_t x = {FLAGS, "name", "default", "help"};` -- fields 1 and
    2 by top-level comma.  Reading "all the string literals in order" instead is
    the trap that made the first cut of this script report vid_renderer's HELP
    TEXT as its default: that one's default is the macro VID_RENDERER_DEFAULT,
    so the second literal in the declaration is the help.  A default that is not
    a plain literal (a macro, a conditional) is reported UNRESOLVABLE rather
    than guessed at -- an unchecked row is honest, a wrongly-checked one is not.
    """
    out, unresolved = {}, set()
    for f in sorted(glob.glob(P('*.c')) + glob.glob(P('*.m'))):
        s = io.open(f, encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'cvar_t\s+\w+\s*=\s*\{(.*?)\}\s*;', s, re.S):
            fields = _split_fields(m.group(1))
            if len(fields) < 3: continue
            nm, dv = fields[1], fields[2]
            if not (nm.startswith('"') and nm.endswith('"')): continue
            name = nm[1:-1]
            if dv.startswith('"') and dv.endswith('"'):
                out[name] = dv[1:-1]
            else:
                unresolved.add(name)
    for n in unresolved: out.pop(n, None)
    cvar_defaults.unresolved = unresolved
    return out


def tier_table():
    """(names, [(cvar, [values...])]) straight out of menu.c."""
    s = io.open(P('menu.c'), encoding='utf-8', errors='replace').read()
    blk = re.search(r'm5_quality_levers\[\]\s*=\s*\{(.*?)\n\};', s, re.S)
    if not blk: return None, None
    rows = []
    for m in re.finditer(r'\{\s*&(\w+),\s*\{([^}]*)\}\s*\}', blk.group(1)):
        vals = [v.strip().rstrip('f') for v in m.group(2).split(',') if v.strip()]
        rows.append((m.group(1), vals))
    nm = re.search(r'm5_quality_names\[[^\]]*\]\s*=\s*\n?\s*\{(.*?)\};', s, re.S)
    names = re.findall(r'"([^"]+)"', nm.group(1)) if nm else []
    return names, rows


def counts():
    """The counts the documents quote, read from the tree."""
    c = {}
    t = io.open(P('tests/QuakeM5Tests.m'), encoding='utf-8', errors='replace').read()
    c['xctest'] = len(re.findall(r'^-\s*\(void\)test\w+', t, re.M))
    p = io.open(P('test/parity-4a.sh'), encoding='utf-8', errors='replace').read()
    m = re.search(r'^FULL_VANTAGES="([^"]*)"', p, re.M)
    c['vantages'] = len(m.group(1).split()) if m else 0
    m = re.search(r'^CORE_VANTAGES="([^"]*)"', p, re.M)
    c['core_vantages'] = len(m.group(1).split()) if m else 0
    names, rows = tier_table()
    c['tiers'] = len(names)
    c['levers'] = len(rows)
    parts, decals = effectinfo_counts()
    c['explosion_particles'] = parts.get('TE_EXPLOSION', 0)
    c['explosion_decals'] = decals.get('TE_EXPLOSION', 0)
    c['teleport_particles'] = parts.get('TE_TELEPORT', 0)
    return c


def effectinfo_counts(quality=3):
    """Above-water particle totals per effect, re-derived from m5/effectinfo.txt.

    The arithmetic is cl_particles.c:1757-1758 -- `countabsolute` passes through
    unscaled and `count` is multiplied by cl_particles_quality.  The documents
    quote Seb's quality 3, so 3 is the constant here and the documents say so in
    words.  `type decal` blocks are counted apart (a decal is not a particle; the
    file's own phrasing is "1 decal + N particles"), and `underwater` blocks are
    left out because every quoted figure is the above-water one.  Every other
    keyword is ignored, so a new one cannot break this.

    Added 2026-09-01 after the file's own header shipped a sum that dropped one
    layer (161 for 191) and two documents copied it.  The claims anchored on it
    live in SETTINGS.md and in the file's header -- NOT in CLAUDE.md's session
    records, which are a dated log: a record that quotes the count of its own day
    is correct as history even after the file changes.
    """
    parts, decals = {}, {}
    st = {'name': None, 'kind': None, 'cabs': 0.0, 'cmul': 0.0, 'under': False}
    def flush():
        if st['name'] is None or st['kind'] is None: return
        if st['kind'] == 'decal':
            decals[st['name']] = decals.get(st['name'], 0) + 1
        elif not st['under']:
            parts[st['name']] = parts.get(st['name'], 0) + int(st['cabs'] + st['cmul'] * quality)
    for raw in io.open(P('m5/effectinfo.txt'), encoding='utf-8', errors='replace'):
        line = raw.split('//', 1)[0].strip()
        if not line: continue
        tok = line.split()
        if tok[0] == 'effect':
            flush()
            st.update(name=tok[1], kind=None, cabs=0.0, cmul=0.0, under=False)
        elif tok[0] == 'type': st['kind'] = tok[1]
        elif tok[0] == 'countabsolute': st['cabs'] = float(tok[1])
        elif tok[0] == 'count': st['cmul'] = float(tok[1])
        elif tok[0] == 'underwater': st['under'] = True
    flush()
    return parts, decals


WORDNUM = {6:'six', 10:'ten', 22:'twenty-two', 35:'thirty-five', 36:'thirty-six',
           37:'thirty-seven', 38:'thirty-eight', 58:'fifty-eight'}


# ------------------------------------------------------------------- 1 checks

def check_defaults(defs):
    """Every Default-column cvar row in SETTINGS.md against the engine.

    Only rows under a header whose SECOND column is literally 'Default' are
    checked -- the tier table's second column is a tier name, and treating it
    as a default is how the first cut of this script produced sixteen
    false positives."""
    s = io.open(P('SETTINGS.md'), encoding='utf-8', errors='replace').read()
    default_col = False
    checked = skipped = 0
    for line in s.split('\n'):
        h = re.match(r'^\|\s*(Setting|Cvar|Command)\s*\|\s*([^|]*?)\s*\|', line)
        if h:
            default_col = (h.group(2).strip().lower() == 'default')
            continue
        if line.startswith('#'):
            default_col = False
            continue
        if not default_col: continue
        m = re.match(r'^\|\s*`([a-z_0-9]+)`\s*\|\s*([^|]*?)\s*\|', line)
        if not m: continue
        name, doc = m.group(1), m.group(2)
        if name in NOT_CVARS: continue
        if name not in defs:
            skipped += 1
            continue
        if not same_value(doc, defs[name]):
            fail('SETTINGS.md', '`%s` Default column says %s, the engine registers "%s"'
                 % (name, doc.strip() or '(blank)', defs[name]))
        checked += 1
    note('SETTINGS.md: %d cvar defaults checked, %d unresolvable' % (checked, skipped))
    if checked < 100:
        fail('docsync', 'only %d default rows were checked -- the table parser has '
                        'stopped matching, which makes this check vacuous' % checked)


def same_value(doc, eng):
    """Compare a documented default with a registered one.

    Strips markdown emphasis and backticks, and accepts the typographic minus
    (SETTINGS.md uses U+2212 in places) as ASCII."""
    t = re.sub(r'\*\*|`', '', doc).strip().strip('"')
    t = t.replace('−', '-').replace('–', '-')
    if t == eng: return True
    try:
        return abs(float(t) - float(eng)) < 1e-9
    except ValueError:
        return False


def check_counts(c):
    claims = [
        ('CLAUDE.md', r'\((\d+) XCTest cases wrapping', 'xctest'),
        ('CLAUDE.md', r'`parity-4a\.sh`\s*\(\*\*([a-z-]+)\*\* vantages', 'vantages'),
        # The effectinfo totals: the reference table and the file's own header.
        # Deliberately no CLAUDE.md anchor -- see effectinfo_counts().
        ('SETTINGS.md', r'`TE_EXPLOSION`[^\n]*\*\*(\d+) particles\*\*', 'explosion_particles'),
        ('SETTINGS.md', r'`TE_TELEPORT`[^\n]*\*\*(\d+) particles\*\*', 'teleport_particles'),
        ('m5/effectinfo.txt', r'Quality-3 total: (\d+) decal \+ \d+ particles', 'explosion_decals'),
        ('m5/effectinfo.txt', r'Quality-3 total: \d+ decal \+ (\d+) particles', 'explosion_particles'),
    ]
    for fn, pat, key in claims:
        s = io.open(P(fn), encoding='utf-8', errors='replace').read()
        m = re.search(pat, s)
        if not m:
            fail('docsync', '%s: the claim matching /%s/ has moved or been reworded -- '
                            'this check is now vacuous and must be re-anchored' % (fn, pat))
            continue
        got, want = m.group(1), c[key]
        ok = (got == str(want)) or (got == WORDNUM.get(want))
        if not ok:
            fail(fn, 'says %r for %s; the tree has %d (%s)'
                 % (got, key, want, WORDNUM.get(want, want)))


def check_tier_names(names):
    """The tier names must appear, in order, wherever the cycle is described."""
    for fn in ('SETTINGS.md', 'docs/GUIDE.html', 'docs/MANUAL.html'):
        s = io.open(P(fn), encoding='utf-8', errors='replace').read()
        seq = [n for n in re.findall(r'\b(' + '|'.join(map(re.escape, names)) + r')\b', s)]
        missing = [n for n in names if n not in seq]
        if missing:
            fail(fn, 'never names the tier(s) %s -- the table has %d tiers'
                 % (', '.join(missing), len(names)))


# Phrases that were CORRECT in the four-tier table and are wrong in a six-tier one.
# Every one of these was found live on 2026-08-30, a day after the table gained
# Superfast and Ultimate: the generated grids had been regenerated and every
# sentence around them still described four tiers.  The membership check below
# catches the SETTINGS table rows; the GUIDE is prose, so it needs this list.
STALE_COUNT_WORDS = [
    (r'all four tiers', 'all four tiers'),
    (r'identical on all four\b', 'identical on all four'),
    (r'\bfour tiers\b', 'four tiers'),
    (r'\bthe four shipped tiers\b', 'the four shipped tiers'),
    (r'\bBest and Better\b', 'Best and Better'),
    (r'\bBetter and Best\b', 'Better and Best'),
    (r'\bGood and Fast\b', 'Good and Fast'),
    (r'\bFast and Good\b', 'Fast and Good'),
    (r'\btop two tiers\b', 'top two tiers'),
]

# Tier names that have been RETIRED.  The table was renamed on 2026-08-01 and
# CLAUDE.md's live cvar reference still called the tiers Performance/Balanced/
# Quality/Ultra a month later, beside a lever count that was also four years of
# levers out of date.
RETIRED_TIER_NAMES = ['Performance', 'Balanced', 'Ultra']


def check_retired_tier_names(names, files):
    """A current-tense document must not name a tier the table no longer has.

    Required context: a slash-joined run ("Performance/Balanced/Quality/Ultra")
    or an explicit "Ultra tier".  A bare capitalised word is not enough -- the
    GUIDE has a section heading and a nav link both reading "Performance", and
    flagging those on the first run is exactly how a checker earns a reputation
    for noise.  Applied only to the reference documents and to CLAUDE.md's
    cvar-reference section, NOT to its session records: a record written when
    the tiers were called Performance..Ultra is correct as history, and
    rewriting it would be falsifying the log."""
    retired = [n for n in RETIRED_TIER_NAMES if n not in names]
    for fn, lo, hi in files:
        L = io.open(P(fn), encoding='utf-8', errors='replace').read().split('\n')
        for i, line in enumerate(L):
            if not (lo <= i + 1 <= hi): continue
            for n in retired:
                e = re.escape(n)
                if re.search(r'/%s\b|\b%s/|\b%s\s+tiers?\b' % (e, e, e), line):
                    fail(fn, 'line %d names the retired tier "%s"; the table is %s'
                         % (i + 1, n, '/'.join(names)))
                    break


NUMBER_WORDS = set('one two three four five six seven eight nine ten eleven twelve '
                   'thirteen fourteen fifteen sixteen seventeen eighteen nineteen twenty '
                   'twenty-one twenty-two twenty-three twenty-four'.split())


def check_lever_count(rows, files):
    """"applies N levers" in prose, against the table.

    ONLY a number may precede "levers".  Matching any word flagged "the same
    levers" and "twenty-two performance levers" on the first run, and the second
    of those was correct."""
    n = len(rows)
    words = {6: 'six', 11: 'eleven', 16: 'sixteen', 22: 'twenty-two', 23: 'twenty-three', 24: 'twenty-four', 25: 'twenty-five'}
    for fn, lo, hi in files:
        L = io.open(P(fn), encoding='utf-8', errors='replace').read().split('\n')
        for i, line in enumerate(L):
            if not (lo <= i + 1 <= hi): continue
            for m in re.finditer(r'\b([\w-]+)\s+(?:perf(?:ormance)?\s+)?levers\b', line):
                w = m.group(1).lower()
                if w not in NUMBER_WORDS and not w.isdigit(): continue
                if w != words.get(n) and w != str(n):
                    fail(fn, 'line %d says "%s levers"; the table has %d' % (i + 1, w, n))


def check_upscale_claim(rows, files):
    """"every tier renders small and upscales" is false the moment one renders native."""
    vs = dict(rows).get('r_viewscale')
    if not vs or max(float(v) for v in vs) < 1.0: return
    for fn in files:
        s = io.open(P(fn), encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'every tier[^.<]{0,60}upscal', s, re.I | re.S):
            fail(fn, 'line %d claims every tier upscales, but a tier renders at '
                     'r_viewscale 1' % (s.count('\n', 0, m.start()) + 1))

def claude_cvar_reference_range():
    """(first, last) line of CLAUDE.md's live '## Cvar reference' section."""
    L = io.open(P('CLAUDE.md'), encoding='utf-8', errors='replace').read().split('\n')
    lo = hi = None
    for i, line in enumerate(L):
        if line.startswith('## Cvar reference'): lo = i + 1
        elif lo is not None and line.startswith('## ') and hi is None: hi = i
    return (lo or 1, hi or len(L))


def check_tier_count_words(names, files):
    """Cheap net for the commonest prose rot: a four-tier phrase in a six-tier tree.

    Two refinements, both paid for by false positives on the first run.  A pair
    phrase is flagged only when it is the WHOLE list -- "off for Superfast, Fast
    and Good" legitimately contains "Fast and Good", and flagging it would train
    the reader to ignore this check.  And the whole thing is skipped for
    CLAUDE.md, whose session records are a dated log: a record written when
    there were four tiers is correct as history."""
    n = len(names)
    if n == 4: return
    tierpat = '|'.join(map(re.escape, names))
    for fn in files:
        s = io.open(P(fn), encoding='utf-8', errors='replace').read()
        for pat, human in STALE_COUNT_WORDS:
            for m in re.finditer(pat, s, re.I):
                if human[0].isupper() and ' and ' in human:
                    # expand over the whole comma/and-joined run of tier names
                    a, b = m.start(), m.end()
                    while True:
                        left = re.search(r'(?:%s)(?:,\s*|\s+and\s+)$' % tierpat, s[max(0, a - 40):a])
                        if not left: break
                        a -= len(left.group(0))
                    right = re.match(r'(?:,\s*|\s+and\s+)(?:%s)' % tierpat, s[b:b + 40])
                    if right: b += len(right.group(0))
                    if len(re.findall(tierpat, s[a:b])) != 2:
                        continue          # part of a longer, possibly correct, list
                line = s.count('\n', 0, m.start()) + 1
                fail(fn, 'line %d says "%s" but the table has %d tiers' % (line, human, n))


def check_lever_membership(names, rows, path='SETTINGS.md'):
    """"on for X and Y, off for Z" claims about a tier lever, against the table.

    This is the one prose shape worth parsing, because it is the shape that
    breaks whenever a tier is inserted: adding Superfast at the BOTTOM and
    Ultimate at the top left `rt_metal_gi` documented as "on for Best and
    Better, off for Good and Fast" when it is on for three tiers and off for
    three.  Only levers that are strictly on/off across the table are checked --
    a graded lever has no membership to claim."""
    table = {cv: vals for cv, vals in rows}
    s = io.open(P(path), encoding='utf-8', errors='replace').read()
    nameset = '|'.join(map(re.escape, names))
    for line in s.split('\n'):
        m = re.match(r'^\|\s*`([a-z_0-9]+)`\s*\|', line)
        if not m: continue
        cv = m.group(1)
        if cv not in table: continue
        vals = [float(v) for v in table[cv]]
        if not set(vals) <= {0.0, 1.0}: continue      # graded: nothing to claim
        on  = {names[i] for i, v in enumerate(vals) if v}
        off = {names[i] for i, v in enumerate(vals) if not v}
        for word, want in (('on', on), ('off', off)):
            for mm in re.finditer(r'\b%s for ((?:%s)(?:[, ]+(?:and )?(?:%s))*)' % (word, nameset, nameset), line):
                claimed = set(re.findall(nameset, mm.group(1)))
                if claimed != want:
                    fail(path, '`%s` is documented as %s for {%s}; the table has %s for {%s}'
                         % (cv, word, ', '.join(sorted(claimed)), word, ', '.join(sorted(want))))


def check_html_balance(path='docs/GUIDE.html'):
    """The GUIDE is hand-edited HTML and nothing else would notice a lost tag.

    Not a parser -- just a per-element open/close tally, which is enough to catch
    the realistic failure (an edit that drops or doubles a closing tag) without
    pretending to validate."""
    txt = io.open(P(path), encoding='utf-8', errors='replace').read()
    tally = {}
    for m in re.finditer(r'<(/?)(p|strong|em|div|section|code|ul|li|table|tr|td|th|a|h1|h2|h3|span)\b[^>]*>', txt):
        tally[m.group(2)] = tally.get(m.group(2), 0) + (-1 if m.group(1) else 1)
    for el, n in sorted(tally.items()):
        if n: fail(path, '<%s> is unbalanced by %+d' % (el, n))


# ------------------------------------------------- 2 generated (sentinel) blocks

BEGIN = '<!-- docsync:%s BEGIN -- generated by test/docsync.py, do not edit by hand -->'
END   = '<!-- docsync:%s END -->'

def gen_tier_table(names, rows):
    out = ['| Setting | ' + ' | '.join(names) + ' |',
           '|' + '---|' * (len(names) + 1)]
    for cv, vals in rows:
        differs = len(set(vals)) > 1
        cells = ['**%s**' % fmt(v) if differs else fmt(v) for v in vals]
        out.append('| `%s` | %s |' % (cv, ' | '.join(cells)))
    return '\n'.join(out)


def fmt(v):
    try:
        f = float(v)
    except ValueError:
        return v
    if f == int(f): return str(int(f))
    return ('%.4f' % f).rstrip('0').rstrip('.')


def gen_refconfig(cfgpath, defs):
    """A snapshot of the owner's live config -- generated, dated, never transcribed.

    The section this replaces was a hand-typed table that went stale twice, the
    second time in the very claim ("glow is at its default, it is NOT amplified")
    that had been written to stop a reader being misled by the first."""
    if not cfgpath or not os.path.exists(cfgpath):
        return None
    import time
    live = {}
    for line in io.open(cfgpath, encoding='utf-8', errors='replace'):
        m = re.match(r'^\s*"?([A-Za-z0-9_]+)"?\s+"([^"]*)"', line.strip())
        if m: live[m.group(1)] = m.group(2)
    stamp = time.strftime('%Y-%m-%d %H:%M', time.localtime(os.path.getmtime(cfgpath)))
    pre = ('m5_', 'rt_metal', 'r_volumetric', 'r_metalfx', 'r_lava', 'r_edr', 'r_redglow',
           'r_lightningbeam_m5', 'r_viewscale', 'r_viewfbo', 'r_hdr', 'r_wateralpha',
           'r_brightness', 'r_gamma_analytic', 'r_bloom', 'r_fxaa', 'v_gamma',
           'v_contrast', 'v_idlesway', 'vid_vsync', 'vid_renderer', 'cl_particles')
    rows = []
    for k in sorted(live):
        if not k.startswith(pre): continue
        d = defs.get(k)
        if d is None: continue
        if not same_value(live[k], d):
            rows.append((k, live[k], d))
    body = [
        '**Generated from `%s`, last written %s.**' % (cfgpath.replace(os.path.expanduser('~'), '~'), stamp),
        'Regenerate with `python3 test/docsync.py --refresh --config <path>`.',
        '',
        'Only cvars whose archived value BEATS the shipped default are listed -- those are',
        'the ones that make his picture differ from a fresh install, and the ones an',
        'investigation needs. Everything absent from this table is at its default.',
        '',
        '| Cvar | His | Shipped default |',
        '|---|---|---|',
    ]
    for k, v, d in rows:
        body.append('| `%s` | **%s** | %s |' % (k, v, d))
    body.append('')
    body.append('%d archived overrides.' % len(rows))
    return '\n'.join(body)


def splice(path, key, content, refresh):
    s = io.open(path, encoding='utf-8', errors='replace').read()
    b, e = BEGIN % key, END % key
    if b not in s or e not in s:
        if refresh:
            fail('docsync', '%s has no docsync:%s sentinels -- add them first' % (path, key))
        return
    head, rest = s.split(b, 1)
    _, tail = rest.split(e, 1)
    want = b + '\n' + content + '\n' + e
    have = b + rest.split(e, 1)[0] + e
    if have.strip() == want.strip():
        return
    if refresh:
        io.open(path, 'w', encoding='utf-8').write(head + want + tail)
        note('%s: regenerated docsync:%s' % (os.path.basename(path), key))
    else:
        fail(os.path.basename(path),
             'the generated block docsync:%s is out of date -- run '
             '`python3 test/docsync.py --refresh`' % key)


# ----------------------------------------------------------------------- main

def main(argv):
    refresh = '--refresh' in argv
    cfg = None
    if '--config' in argv:
        cfg = argv[argv.index('--config') + 1]

    defs = cvar_defaults()
    names, rows = tier_table()
    if not names:
        print('docsync: FAIL -- could not parse m5_quality_levers[] from menu.c')
        return 1
    c = counts()

    check_defaults(defs)
    check_counts(c)
    check_tier_names(names)
    check_tier_count_words(names, ['SETTINGS.md', 'docs/GUIDE.html', 'docs/MANUAL.html'])
    check_lever_membership(names, rows)
    # CLAUDE.md's cvar reference is current-tense; its session records are a
    # dated log and are deliberately excluded from every prose check here.
    cvarref = claude_cvar_reference_range()
    check_retired_tier_names(names, [('SETTINGS.md', 1, 10**9),
                                     ('docs/GUIDE.html', 1, 10**9),
                                     ('docs/MANUAL.html', 1, 10**9),
                                     ('CLAUDE.md',) + cvarref])
    check_lever_count(rows, [('SETTINGS.md', 1, 10**9),
                             ('docs/GUIDE.html', 1, 10**9),
                             ('docs/MANUAL.html', 1, 10**9),
                             ('CLAUDE.md',) + cvarref])
    check_upscale_claim(rows, ['SETTINGS.md', 'docs/GUIDE.html', 'docs/MANUAL.html'])
    check_html_balance()
    check_html_balance('docs/MANUAL.html')   # the exact working manual, same net
    splice(P('SETTINGS.md'), 'tiertable', gen_tier_table(names, rows), refresh)
    if cfg:
        rc = gen_refconfig(cfg, defs)
        if rc is None:
            fail('docsync', 'config %s not readable' % cfg)
        else:
            splice(P('CLAUDE.md'), 'refconfig', rc, refresh)

    for n in notes: print('docsync: ' + n)
    if failures:
        for f in failures: print('docsync: FAIL -- ' + f)
        print('docsync: %d problem(s)' % len(failures))
        return 1
    print('docsync: PASS -- %d tiers, %d levers, %d XCTest cases, %d vantages, '
          'effectinfo explosion %d+%d teleport %d'
          % (c['tiers'], c['levers'], c['xctest'], c['vantages'],
             c['explosion_decals'], c['explosion_particles'], c['teleport_particles']))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
