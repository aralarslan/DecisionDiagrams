# Fits log2(states) against front size F and edge cut C from level_lab rows
# (HANDOFF.md §5.9) and evaluates ordering selectors (BFS vs cut-width).
# usage: python3 fit_levels.py levels.txt
import numpy as np, collections, sys
# Runs stopped by their time limit can leave a truncated last row: keep complete rows only.
rows=[r for r in (l.split() for l in open(sys.argv[1])) if len(r)==8 and all(x.isdigit() for x in r[3:7])]
data=collections.defaultdict(list)
for case,st,pv,l,F,C,n,t in rows:
    data[(case,st,pv)].append((int(l),int(F),int(C),int(n)))
L={}
for (case,st,pv),v in data.items(): L[case]=max(L.get(case,0),max(x[0] for x in v))
complete={k:v for k,v in data.items() if max(x[0] for x in v)==L[k[0]]}
fe={}
for pv in ('0.9','1.0'):
    cases=sorted(set(k[0] for k in data if k[2]==pv)); idx={c:i for i,c in enumerate(cases)}
    X=[];y=[]
    for (case,st,p),v in data.items():
        if p!=pv: continue
        for l,F,C,n in v:
            if n>0:
                row=[F,C]+[0]*len(cases); row[2+idx[case]]=1; X.append(row); y.append(np.log2(n))
    X=np.array(X,float);y=np.array(y)
    coef=np.linalg.lstsq(X,y,rcond=None)[0]; pred=X@coef
    print(f"pv={pv} fixed effects: log2 s = {coef[0]:.3f}F + {coef[1]:.3f}C + c_graph   R2={1-((y-pred)**2).sum()/((y-y.mean())**2).sum():.3f}")
    fe[pv]=coef[:2]
def evaluate(model_of, show=False):
    hits=0;tot=0;loss=[];bad=[]
    for pv in ('0.9','1.0'):
        for case in sorted(set(k[0] for k in complete if k[2]==pv)):
            cands={st:complete[(case,st,pv)] for st in ('bfs','cutwidth') if (case,st,pv) in complete}
            if len(cands)<2: continue
            actual={st:sum(x[3] for x in v) for st,v in cands.items()}
            score={st:model_of(pv,v) for st,v in cands.items()}
            best=min(actual,key=actual.get); pick=min(score,key=score.get)
            tot+=1; hits+=best==pick; loss.append(actual[pick]/actual[best])
            if best!=pick: bad.append(f"{pv} {case} picks {pick} ({actual[pick]/actual[best]:.2f}x)")
    if show: print("   misses:", "; ".join(bad))
    return hits,tot,round(max(loss),3),round(float(np.exp(np.mean(np.log(loss)))),4)
cur=lambda pv,v:(max(x[2] for x in v),max(x[1] for x in v),sum(x[2] for x in v))
print("current (maxcut,maxF,sumcut):",evaluate(cur,True))
work=lambda pv,v:sum(2**(fe[pv][0]*x[1]+fe[pv][1]*x[2]) for x in v)
print("FE model total work:",evaluate(work,True))
for a,b in ((1.5,0.5),(1.0,0.5),(1.0,1.0),(1.2,0.3),(1.5,0.3)):
    w=lambda pv,v,a=a,b=b:sum(2**(a*x[1]+b*x[2]) for x in v)
    print(f"a={a} b={b}:",evaluate(w))
print("---- regime rule (K_n excluded: same vertex order, tie in the real selector)")
def ev(rule):
    hits=0;tot=0;loss=[];bad=[]
    for pv in ('0.9','1.0'):
        for case in sorted(set(k[0] for k in complete if k[2]==pv)):
            if case.startswith('K'): continue
            cands={st:complete[(case,st,pv)] for st in ('bfs','cutwidth') if (case,st,pv) in complete}
            if len(cands)<2: continue
            actual={st:sum(x[3] for x in v) for st,v in cands.items()}
            score={st:rule(pv,v) for st,v in cands.items()}
            best=min(actual,key=actual.get); pick=min(score,key=score.get)
            tot+=1; hits+=best==pick; loss.append(actual[pick]/actual[best])
            if best!=pick: bad.append(f"{pv} {case} {actual[pick]/actual[best]:.2f}x")
    return hits,tot,round(max(loss),3),round(float(np.exp(np.mean(np.log(loss)))),4),bad
print("current:",ev(cur))
for a in (1.0,1.3,1.61,2.0):
    rule=lambda pv,v,a=a: (sum(2**(a*x[1]) for x in v),) if pv=='1.0' else cur(pv,v)
    print(f"perfect: work est a={a}:",ev(rule))
