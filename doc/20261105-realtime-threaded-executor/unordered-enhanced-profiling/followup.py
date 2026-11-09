import subprocess,json,random,statistics
from pathlib import Path
r=Path('/tmp/enhanced-unordered');rows=[];rng=random.Random(20261109)
for case,extra in [('p1',['--producers','1','--messages','500000']),('p4',['--producers','4','--messages','500000']),('latency',['--scenario','latency','--messages','5000']),('large4',['--producers','4','--messages','500000','--payload','256'])]:
 for repeat in range(5):
  labels=['single','notoken','fence'];rng.shuffle(labels)
  for label in labels:
   cmd=['taskset','-c','20-27',str(r/label/'bench'),'--executor','enhanced','--producer-cpus','20,21,22,23','--consumer-cpus','24,25,26,27']+extra
   row=json.loads(subprocess.check_output(cmd,text=True,timeout=60));assert row['ok'];row.update(label=label,case=case,repeat=repeat,command=cmd);rows.append(row)
 print(case,flush=True)
(r/'followup.json').write_text(json.dumps(rows,indent=2))
for case in dict.fromkeys(x['case'] for x in rows):
 for label in ['single','notoken','fence']:
  data=[x for x in rows if x['case']==case and x['label']==label]
  print(case,label, 'cpu',round(statistics.median(x['process_cpu_ns']/x['items'] for x in data)), 'wall',round(statistics.median(x['wall_ns']/x['items'] for x in data)),'p50',statistics.median(x['p50_ns'] for x in data),flush=True)
