# Builds VoiceLinesDefault.inc (the built-in lines compiled into VoiceCommand.exe) from
# VoiceLines.default.txt. Edit the .txt, run "python gen_voicelines.py" in this folder, rebuild.
# Format: "# note" line(s), then "[group]", then one line per row (see the header VoiceCommand
# writes into VoiceLines.txt). The last note line before a group becomes its description.
import os
here = os.path.dirname(os.path.abspath(__file__))
groups, about, cur = [], '', None
for l in open(os.path.join(here, 'VoiceLines.default.txt'), encoding='utf-8').read().splitlines():
    s = l.strip()
    if not s:
        continue
    if s.startswith('#'):
        about = s[1:].strip()
        continue
    if s.startswith('[') and s.endswith(']'):
        cur = [s[1:-1], about, []]
        groups.append(cur)
        about = ''
        continue
    if cur is None:
        raise SystemExit('line before any [group]: ' + s)
    cur[2].append(s)
keys = [g[0] for g in groups]
dups = sorted({k for k in keys if keys.count(k) > 1})
if dups:
    raise SystemExit('duplicate groups: ' + ', '.join(dups))
esc = lambda x: x.replace('\\', '\\\\').replace('"', '\\"')
out = ['// GENERATED from VoiceLines.default.txt by gen_voicelines.py - edit that file, then re-run the script.']
for k, ab, ls in groups:
    out.append('{L"%s", L"%s",\n {%s}},' % (k, esc(ab), ', '.join('L"%s"' % esc(x) for x in ls)))
open(os.path.join(here, 'VoiceLinesDefault.inc'), 'w', encoding='utf-8', newline='\n').write('\n'.join(out) + '\n')
print('%d groups, %d lines -> VoiceLinesDefault.inc' % (len(groups), sum(len(g[2]) for g in groups)))
