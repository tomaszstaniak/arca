#!/usr/bin/env python3
"""Create complete dependency-source and per-ABI LGPL relink release assets."""
import argparse,hashlib,io,json,os,re,shutil,subprocess,tarfile,tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('version');p.add_argument('--libiconv-tar',required=True);p.add_argument('--targets',default='v1,abiv11,raspi',help='comma-separated: v1, abiv11, raspi (a release of some targets only)');args=p.parse_args()
ALL=(('v1','abiv1','x86_64','pc'),('abiv11','abiv11','x86_64','pc'),('raspi','abiv1','aarch64','raspi'))
wanted=args.targets.split(',')
assert wanted and all(w in [a[0] for a in ALL] for w in wanted), args.targets
TARGETS=tuple(a for a in ALL if a[0] in wanted)
r=Path(__file__).resolve().parents[1];out=r/'dist';out.mkdir(exist_ok=True)
revision=subprocess.check_output(['git','-C',str(r),'rev-parse','HEAD'],text=True).strip()
outputs=[out/f'Arca-{args.version}-source.tar.gz']+[out/f'Arca-{args.version}-relink-aros-{arch}-{abi}-{platform}.tar.gz' for _,abi,arch,platform in TARGETS]
for target in outputs:
 if target.exists():raise SystemExit(f'{target} already exists')

iconv=Path(args.libiconv_tar)
assert hashlib.sha256(iconv.read_bytes()).hexdigest()=='72b24ded17d687193c3366d0ebe7cde1e6b18f0df8c55438ac95be39e8a30613'
with tempfile.TemporaryDirectory() as d:
 t=Path(d);root=t/f'Arca-{args.version}-source';root.mkdir()
 raw=subprocess.check_output(['git','-C',str(r),'archive','--format=tar','HEAD'])
 with tarfile.open(fileobj=io.BytesIO(raw)) as z:
  z.extractall(root,members=[m for m in z if Path(m.name).name.lower() not in ('agents.md','claude.md','project.json')],filter='data')
 deps=root/'DependencySources';deps.mkdir()
 pins={}
 for line in (r/'deps.lock').read_text().splitlines():
  if line.strip() and not line.startswith('#'):
   name,url,pin,*_=line.split();pins['deps/'+name]={'url':url,'revision':pin}
 for name,v in json.loads((r/'upstreams.json').read_text())['repositories'].items():pins['upstream/'+name]={'url':v['url'],'revision':v['commit']}
 for key,v in pins.items():
  cmd=['git','-C',str(r/key),'archive','--format=tar',v['revision']]
  if key.startswith('upstream/contrib'):cmd+=['workbench/libs/xad']
  data=subprocess.check_output(cmd)
  cleaned=io.BytesIO();omitted=[]
  with tarfile.open(fileobj=io.BytesIO(data),mode='r:') as original, tarfile.open(fileobj=cleaned,mode='w:') as public:
   for member in original:
    if Path(member.name).name.lower() in ('agents.md','claude.md','project.json'):
     omitted.append(member.name);continue
    public.addfile(member,original.extractfile(member) if member.isfile() else None)
  data=cleaned.getvalue();v['omitted_maintainer_files']=omitted
  target=deps/(key.replace('/','-')+'.tar.gz')
  import gzip
  with target.open('wb') as f:
   with gzip.GzipFile(filename='',mode='wb',fileobj=f,mtime=0) as gz:gz.write(data)
  v['archive']=target.name;v['sha256']=hashlib.sha256(target.read_bytes()).hexdigest()
 with tarfile.open(iconv) as archive:
  forbidden=[m.name for m in archive if Path(m.name).name.lower() in ('agents.md','claude.md','project.json')]
  if forbidden:raise SystemExit(f'forbidden maintainer files in libiconv source: {forbidden}')
 shutil.copyfile(iconv,deps/'libiconv-1.14.tar.gz')
 (deps/'sources.json').write_text(json.dumps(pins,indent=2)+'\n')
 (deps/'README.txt').write_text('Pinned application dependency sources (LLVM compiler-runtime source is linked by URL and hash in ../experiments/7z-backend/llvm-runtime/README.md, not bundled). Maintainer-only AGENTS.md, CLAUDE.md and project.json files are omitted; sources.json records the exclusions. Program and library source contents are unchanged. Apply the matching ordered patches in ../patches for XAD and LHa. libiconv1.14 is unmodified. Standard build scripts fetch the same public Git pins; these archives provide the complete source independently of those hosts. Source tarballs omit Git metadata.\n')
 target=out/f'Arca-{args.version}-source.tar.gz'
 if target.exists():raise SystemExit(f'{target} already exists')
 with tarfile.open(target,'w:gz') as z:z.add(root,arcname=root.name)
 # Derive the object lists from the actual application's authoritative build recipe.
 recipe=(r/'scripts/build.sh').read_text();loops=re.findall(r'for f in ([^;]+); do',recipe)
 cli=[Path(x).name+'.o' for x in loops[0].split()]+['xad_static.o']
 gui=[x for x in cli if x!='main.o']+['gui_'+Path(x).name+'.o' for x in loops[1].split()]
 gui += [x+'.o' for x in re.search(r'for c in ([^;]+); do',recipe).group(1).split()]
 for abi,targetabi,arch,platform in TARGETS:
  cross=f'{arch}-aros'
  toolchain=Path(os.environ[{'v1':'UNARC_TOOLCHAIN_V1','abiv11':'UNARC_TOOLCHAIN_ABIV11','raspi':'UNARC_TOOLCHAIN_RASPI'}[abi]])
  strip=str(toolchain/f'{cross}-strip')
  root=t/f'Arca-{args.version}-relink-aros-{arch}-{targetabi}-{platform}';root.mkdir();(root/'objects').mkdir();(root/'lib').mkdir()
  b=r/'build'/f'{abi}-{arch}'
  for n in set(cli+gui):
   shutil.copyfile(b/'obj'/n,root/'objects'/n)
   subprocess.run([strip,'--strip-debug',str(root/'objects'/n)],check=True)
  sdk=Path(os.environ[{'v1':'UNARC_SDK_V1','abiv11':'UNARC_SDK_ABIV11','raspi':'UNARC_SDK_RASPI'}[abi]])
  for n in ('libarchive.a','libxadstatic.a','liblzma.a'):
   shutil.copyfile(b/'prefix/lib'/n,root/'lib'/n)
   subprocess.run([strip,'--strip-debug',str(root/'lib'/n)],check=True)
  for n in ('libbz2_nostdio.a','libz.static.a')+(('libiconv.a',) if abi=='abiv11' else ()):
   shutil.copyfile(sdk/'lib'/n,root/'lib'/n)
   subprocess.run([strip,'--strip-debug',str(root/'lib'/n)],check=True)
  (root/'cli.objects').write_text('\n'.join('objects/'+x for x in cli)+'\n');(root/'gui.objects').write_text('\n'.join('objects/'+x for x in gui)+'\n')
  libs='lib/libarchive.a lib/libxadstatic.a lib/liblzma.a lib/libbz2_nostdio.a lib/libz.static.a'+(' lib/libiconv.a' if abi=='abiv11' else '')+' -lpthread'
  compiler=f'{cross}-clang' if abi=='raspi' else f'{cross}-gcc'
  setup='export RPI_SDK="$ARCA_SDK"\n' if abi=='raspi' else ''
  script = '#!/bin/bash\nset -euo pipefail\ncd "$(dirname "$0")"\n'
  script += f': "${{ARCA_TOOLCHAIN:?directory containing {compiler}}}" "${{ARCA_SDK:?matching SDK Developer directory}}"\n' + setup
  for program,objects in (('arca-cli','cli.objects'),('Arca','gui.objects')):
   script += f'"$ARCA_TOOLCHAIN/{compiler}" --sysroot="$ARCA_SDK" -o {program} @{objects} {libs}\n'
  script += f'for program in arca-cli Arca; do\n  undef=$("$ARCA_TOOLCHAIN/{cross}-nm" -u "$program" | grep -v \' w \' || true)\n  if [ -n "$undef" ]; then echo "unresolved symbols in $program: $undef"; exit 1; fi\ndone\n'
  (root/'relink.sh').write_text(script)
  (root/'relink.sh').chmod(0o755)
  (root/'README.txt').write_text(f'Arca {args.version}, source {revision}, {targetabi} {arch} {platform}. Set ARCA_TOOLCHAIN and ARCA_SDK, replace the LGPL library archive you wish to modify, then bash relink.sh. Application and LGPL library sources are in Arca-{args.version}-source.tar.gz. Debug sections are removed from the supplied relink objects. They retain code, relocations and symbols required to link replacement libraries.\n')
  target=out/(root.name+'.tar.gz')
  if target.exists():raise SystemExit(f'{target} already exists')
  with tarfile.open(target,'w:gz') as z:z.add(root,arcname=root.name)
print('Created complete source and',len(TARGETS),'application relink asset(s) for',revision)
