import subprocess,json,statistics,hashlib,random
from pathlib import Path
r=Path('/tmp/moodycamel-unify');w=Path('/data/hxli/codes/tm_kit/worktrees/realtime-threaded-executor');rows=[];rng=random.Random(20261109)
bins={'old':r/'base/bench','merged':w/'tm_basic/builddir_release/test/executor_bench/executor_bench_moody'}
for case,extra in [('p1',['--producers','1','--messages','500000']),('p4',['--producers','4','--messages','500000']),('large4',['--producers','4','--messages','500000','--payload','256']),('latency',['--scenario','latency','--messages','5000'])]:
 for repeat in range(5):
  labels=list(bins);rng.shuffle(labels)
  for label in labels:
   cmd=['taskset','-c','20-27',str(bins[label]),'--executor','moody','--producer-cpus','20,21,22,23','--consumer-cpus','24,25,26,27']+extra
   row=json.loads(subprocess.check_output(cmd,text=True,timeout=60));assert row['ok'];row.update(label=label,case=case,repeat=repeat,command=cmd,binary_sha256=hashlib.sha256(bins[label].read_bytes()).hexdigest());rows.append(row)
 print(case,flush=True)
(r/'comparison.json').write_text(json.dumps(rows,indent=2))
for case in dict.fromkeys(x['case'] for x in rows):
 for label in bins:
  data=[x for x in rows if x['case']==case and x['label']==label]
  print(case,label,'CPU',round(statistics.median(x['process_cpu_ns']/x['items'] for x in data)),'wall',round(statistics.median(x['wall_ns']/x['items'] for x in data)),'p50',statistics.median(x['p50_ns'] for x in data),'p99',statistics.median(x['p99_ns'] for x in data),flush=True)
