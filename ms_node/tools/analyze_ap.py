import json

# CH data
ch_seqs = [3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,46,47,48,49,50,51,52,53,54,55,56,57,58,59,60,61,62]

# MBR1 data - seq and ts pairs
mbr1 = [(0,78),(1,7533),(2,12582),(3,17632),(4,22682),(6,30867),(7,31958),(11,34327),(14,35917),(15,37005),(18,38050),(19,43714),(20,48854),(21,53954),(22,59054),(24,65147),(27,66737),(28,67806),(32,70239),(35,71839),(36,72893),(40,75237),(43,76827),(44,78538),(45,83686),(46,88795),(47,93902),(48,98995),(50,105268),(54,107610),(58,110300),(59,115415),(60,120548),(61,125713),(62,130836),(63,135975),(64,141075),(65,146175),(66,151275),(67,156375),(69,162597),(71,163482),(73,165288),(77,167660),(78,173315),(79,178415),(81,184076),(83,185967),(87,188310),(89,189162),(90,190873),(91,195965),(92,201065),(93,206225),(94,211388),(95,216505),(96,221655),(98,228377)]

# MBR2 data
mbr2 = [(0,78),(1,7572),(2,12662),(3,17722),(4,22772),(7,30999),(8,36094),(9,41212),(10,46305),(11,51404),(12,56564),(13,61673),(14,66775),(15,71881),(16,76990),(18,83809),(22,86137),(24,87022),(25,88708),(26,93805),(27,98905),(31,106057),(34,107647),(35,108725),(40,111350),(41,116495),(42,121611),(43,126716),(44,131835),(45,137042),(46,142185),(47,148132),(49,149967)]

print("=" * 60)
print("CH (3125565838)")
print(f"  Records: {len(ch_seqs)}")
print(f"  Seq range: {ch_seqs[0]} -> {ch_seqs[-1]}")
expected = list(range(ch_seqs[0], ch_seqs[-1]+1))
missing = sorted(set(expected) - set(ch_seqs))
print(f"  Missing ({len(missing)}): {missing}")
print(f"  Coverage: {len(ch_seqs)}/{len(expected)} = {100*len(ch_seqs)/len(expected):.1f}%")

for name, pairs in [("MBR1 (3125668638)", mbr1), ("MBR2 (3125683842)", mbr2)]:
    seqs = [s for s,t in pairs]
    tss = [t for s,t in pairs]
    print(f"\n{'='*60}")
    print(f"{name}")
    print(f"  Records: {len(seqs)}")
    print(f"  Seq range: {seqs[0]} -> {seqs[-1]}")
    expected = list(range(seqs[0], seqs[-1]+1))
    missing = sorted(set(expected) - set(seqs))
    print(f"  Missing ({len(missing)}): {missing}")
    print(f"  Coverage: {len(seqs)}/{len(expected)} = {100*len(seqs)/len(expected):.1f}%")
    
    # Classify as LIVE (~5s gap) vs BURST (<2s gap)
    live_seqs = []
    burst_seqs = []
    print(f"\n  {'seq':>5} {'ts':>8} {'td':>6} {'type':>6}")
    for i in range(len(pairs)):
        s, t = pairs[i]
        td = t - pairs[i-1][1] if i > 0 else 0
        if td > 3000:
            typ = "LIVE"
            live_seqs.append(s)
        elif i > 0:
            typ = "BURST"
            burst_seqs.append(s)
        else:
            typ = "START"
        print(f"  {s:>5} {t:>8} {td:>6} {typ:>6}")
    
    print(f"\n  LIVE sends: {len(live_seqs)} seqs: {live_seqs}")
    print(f"  BURST drain: {len(burst_seqs)} seqs: {burst_seqs}")
    
    # Find gaps in LIVE sends
    if live_seqs:
        live_expected = list(range(live_seqs[0], live_seqs[-1]+1))
        live_missing = sorted(set(live_expected) - set(live_seqs))
        print(f"  LIVE missing: {live_missing}")
