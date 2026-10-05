#!/usr/bin/env python3
"""release/check-recipes.py -- the public download's recipe allow-list, checked.

A recipe .cfg ships in the public download because it is NAMED in release/recipes.txt
(REVIEW.md 0.9; RELEASE.md's allow-list rule), and the documents a player is sent to --
docs/GUIDE.html and docs/MANUAL.html, which travel with the download, and SETTINGS.md, the
full reference both of them point at -- must not name one that is not there.

  python3 release/check-recipes.py            # the tree: every listed recipe is a tracked
                                              # file in m5/, and the docs are covered
  python3 release/check-recipes.py --list     # the [ship] names, one per line
  python3 release/check-recipes.py --dir D [--docs X]
                                              # a built packs/m5: exactly the list, plus the
                                              # profile the build writes, and the docs in X
                                              # covered
Both modes: every recipe the docs name is shipped, listed [not-shipped] with its reason,
GENERATED (the profile) or EXTERNAL (id1's own); every recipe a shipped recipe names (its
"exec x_off.cfg to revert") ships too. Shorthand in the docs ("x_off.cfg / _on.cfg")
resolves against the nearest full name before it, or fails. test/docsync.py runs the tree
check, so smoke does; release/make-release.sh runs both.

Tree mode also LINTS every shipped recipe (2026-10-03): each statement must begin with a
cvar or command the engine registers (or an alias the recipe defines). Statements split
the way the console splits them -- on newlines and on every ';' outside quotes, with '//'
comments stripped outside quotes -- so an unquoted echo carrying a ';' is caught: its tail
runs as a command (ball_v2.cfg ran "ball_v1.cfg is the first tuning)" and printed
"Unknown command"). A cvar retired from the engine is caught the same way.
"""
import io, os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIST = os.path.join(ROOT, 'release', 'recipes.txt')
DOCS = ('GUIDE.html', 'MANUAL.html')          # read from --docs in zip mode (they travel with it)
ROOT_DOCS = ('SETTINGS.md',)                  # the reference both send readers to: read from the tree always
GENERATED = ('config.cfg',)                    # written by the build: the public profile
EXTERNAL = ('default.cfg', 'autoexec.cfg')     # id1's own, from the player's paks: never in packs/m5
NAME = re.compile(r'(?<![A-Za-z0-9_])(_?[A-Za-z0-9][A-Za-z0-9_]*)\.cfg\b')
RECIPE = re.compile(r'[A-Za-z0-9][A-Za-z0-9_]*\.cfg')

def load():
    ship, notship, sec, errs = [], {}, None, []
    for i, raw in enumerate(io.open(LIST, encoding='utf-8'), 1):
        line = raw.split('#', 1)[0].strip()
        if not line:
            continue
        if line in ('[ship]', '[not-shipped]'):
            sec = line; continue
        name, _, reason = line.partition(' ')
        if not RECIPE.fullmatch(name):
            errs.append('recipes.txt:%d: %r is not a recipe name' % (i, name)); continue
        if sec == '[ship]':
            ship.append(name)
        elif sec == '[not-shipped]':
            if not reason.strip():
                errs.append('recipes.txt:%d: %s is not shipped but gives no reason' % (i, name))
            notship[name] = reason.strip()
        else:
            errs.append('recipes.txt:%d: %s sits before any [section]' % (i, name))
    dup = sorted({n for n in ship if ship.count(n) > 1} | (set(ship) & set(notship)))
    if dup:
        errs.append('recipes.txt: listed twice (or both shipped and not): %s' % ' '.join(dup))
    return ship, notship, errs

def named_by_docs(docdir):
    """{name: 'FILE:line'} for every recipe the docs name; '_on.cfg' shorthand resolves
    against the nearest full name before it (within 300 characters) or is an error."""
    out, errs = {}, []
    for path, fn in ([(os.path.join(docdir, f), f) for f in DOCS] +
                     [(os.path.join(ROOT, f), f) for f in ROOT_DOCS]):
        txt = io.open(path, encoding='utf-8', errors='replace').read()
        last = None
        for m in NAME.finditer(txt):
            tok, where = m.group(1), '%s:%d' % (fn, txt.count('\n', 0, m.start()) + 1)
            if tok.startswith('_'):
                if not last or m.start() - last[1] > 300:
                    errs.append('%s: shorthand %s.cfg has no full name before it' % (where, tok)); continue
                name = re.sub(r'_[A-Za-z0-9]+$', '', last[0]) + tok + '.cfg'
            else:
                name = tok + '.cfg'
                last = (tok, m.end())
            out.setdefault(name, where)
    return out, errs

def registered():
    """Every cvar and command name the engine registers, read from the source."""
    names = set()
    for f in os.listdir(ROOT):
        if not f.endswith(('.c', '.m')):
            continue
        s = io.open(os.path.join(ROOT, f), encoding='utf-8', errors='replace').read()
        names |= set(re.findall(r'cvar_t\s+\w+\s*=\s*\{[^,]*,\s*"([^"]+)"', s))
        names |= set(re.findall(r'Cvar_Get\s*\([^,]*,\s*"([^"]+)"', s))
        names |= set(re.findall(r'Cmd_AddCommand\s*\([^,]*,\s*"([^"]+)"', s))
        names |= set(re.findall(r'Cvar_RegisterVirtual\s*\([^,]*,\s*"([^"]+)"', s))
    return names

def statements(text):
    """The console's own split, transcribed from Cbuf_ParseText (cmd.c): a statement ends
    at a newline or at a ';' outside quotes and outside a comment; a quote preceded by a
    backslash does not toggle; '//' opens a comment only outside quotes and at the start
    of the line or after whitespace. Yields (line number, first token)."""
    line, cur, quotes, comment, pos = 1, [], False, False, 0
    def first(c):
        t = ''.join(c).split()
        return t[0] if t else None
    while pos < len(text):
        ch = text[pos]
        if ch in ';\n\r' and not (ch == ';' and (comment or quotes)):
            yield line, first(cur)
            cur, quotes, comment = [], False, False
            if ch == '\n':
                line += 1
            pos += 1
            continue
        if ch == '/' and not quotes and text[pos + 1:pos + 2] == '/' and (pos == 0 or text[pos - 1] in ' \t\n\r'):
            comment = True
        elif ch == '"' and not comment and (pos == 0 or text[pos - 1] != '\\'):
            quotes = not quotes
        if not comment:
            cur.append(ch)
        pos += 1
    yield line, first(cur)

def lint(ship, src):
    errs, names = [], registered()
    for n in ship:
        p = os.path.join(src, n)
        if not os.path.isfile(p):
            continue
        txt = io.open(p, encoding='utf-8', errors='replace').read()
        aliases = set(re.findall(r'(?m)^\s*alias\s+(\S+)', txt))
        for line, tok in statements(txt):
            if tok is None:
                continue
            tok = tok.strip('"')
            if tok in names or tok in aliases:
                continue
            errs.append('%s:%d runs "%s", which the engine does not register (an unquoted ";" '
                        'in an echo, a typo, or a retired cvar)' % (n, line, tok))
    return errs

def check(target=None, docdir=None):
    """Failures as strings. target None = the tree (m5/, tracked); else a built packs/m5."""
    ship, notship, errs = load()
    docdir = docdir or os.path.join(ROOT, 'docs')
    named, e2 = named_by_docs(docdir)
    errs += e2
    src = target or os.path.join(ROOT, 'm5')
    for n in ship:
        if not os.path.isfile(os.path.join(src, n)):
            errs.append('%s is listed to ship but %s has no such file' % (n, src))
    for n, where in sorted(named.items()):
        if n not in ship and n not in notship and n not in GENERATED and n not in EXTERNAL:
            errs.append('%s names %s, which is neither shipped nor listed as not shipped' % (where, n))
    for n in ship:
        p = os.path.join(src, n)
        if not os.path.isfile(p):
            continue
        for r in sorted(set(RECIPE.findall(io.open(p, encoding='utf-8', errors='replace').read())) - {n}):
            if r not in ship and r not in GENERATED and r not in EXTERNAL:
                errs.append('%s (shipped) points at %s, which does not ship' % (n, r))
    if target:
        have = {f for f in os.listdir(target) if f.endswith('.cfg')}
        extra = sorted(have - set(ship) - set(GENERATED))
        if extra:
            errs.append('%s carries recipes that are not on the list: %s' % (target, ' '.join(extra)))
        for g in GENERATED:
            if g not in have:
                errs.append('%s has no %s' % (target, g))
        for x in EXTERNAL:
            if x in have:
                errs.append('%s carries %s, which would shadow the one in id1' % (target, x))
    else:
        errs += lint(ship, src)
        try:
            tracked = set(subprocess.run(['git', '-C', ROOT, 'ls-files', '-z', '--', 'm5'],
                          capture_output=True, check=True).stdout.decode().split('\0'))
            for n in ship:
                if 'm5/' + n not in tracked:
                    errs.append('m5/%s is listed to ship but is not tracked (git add -f it)' % n)
        except (OSError, subprocess.CalledProcessError):
            pass
    return errs

if __name__ == '__main__':
    a = sys.argv[1:]
    if '--list' in a:
        ship, _, errs = load()
        if errs:
            print('\n'.join(errs), file=sys.stderr); sys.exit(1)
        print('\n'.join(ship)); sys.exit(0)
    target = a[a.index('--dir') + 1] if '--dir' in a else None
    docdir = a[a.index('--docs') + 1] if '--docs' in a else None
    errs = check(target, docdir)
    for e in errs:
        print('check-recipes: FAIL -- ' + e)
    if errs:
        sys.exit(1)
    ship, notship, _ = load()
    print('check-recipes: PASS -- %d shipped, %d named but not shipped (%s)'
          % (len(ship), len(notship), target or 'the tree'))
