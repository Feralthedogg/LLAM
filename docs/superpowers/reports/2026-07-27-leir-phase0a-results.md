# LEIR Phase 0A Userspace Advancement Evidence

- Phase: `screen`
- Verdict: **INCONCLUSIVE**
- Meaning: `ADVANCE_PASS` authorizes only the next LEIR research phase; it is not `CATEGORY` and is not a production readiness verdict.

## Decision targets

| Target | Final LEIR spec | Phase 0A continuation |
|---|---:|---:|
| Core wall speedup floor | 1.50x | 1.25x |
| Core CPU ratio ceiling | 0.70x | 0.85x |
| Short-region wall floor | 0.95x | 0.95x |
| Short-region CPU ceiling | 1.05x | 1.05x |
| Terminal/service p99 ratio ceiling | 1.10x | 1.10x |
| Paired wall/CPU spread ceiling | 1.10x | 1.10x |

## Classifier reasons

- wall spread above 1.10x for ('framed_rpc', 1, 1, 64, 1)
- wall spread above 1.10x for ('framed_rpc', 1, 1, 64, 8)
- wall spread above 1.10x for ('framed_rpc', 1, 1, 64, 32)
- wall spread above 1.10x for ('framed_rpc', 1, 1, 1024, 1)
- CPU spread above 1.10x for ('framed_rpc', 1, 1, 1024, 1)
- wall spread above 1.10x for ('framed_rpc', 1, 1, 1024, 8)
- wall spread above 1.10x for ('framed_rpc', 1, 1, 1024, 32)
- CPU spread above 1.10x for ('framed_rpc', 1, 1, 1024, 32)
- wall spread above 1.10x for ('framed_rpc', 2, 1, 64, 1)
- wall spread above 1.10x for ('framed_rpc', 2, 1, 1024, 1)
- wall spread above 1.10x for ('framed_rpc', 2, 1, 1024, 8)
- CPU spread above 1.10x for ('framed_rpc', 2, 1, 1024, 8)
- wall spread above 1.10x for ('framed_rpc', 2, 1, 1024, 32)
- CPU spread above 1.10x for ('framed_rpc', 2, 512, 64, 1)
- wall spread above 1.10x for ('framed_rpc', 2, 512, 64, 32)
- CPU spread above 1.10x for ('framed_rpc', 2, 512, 64, 32)
- wall spread above 1.10x for ('framed_rpc', 4, 1, 64, 1)
- wall spread above 1.10x for ('framed_rpc', 4, 1, 1024, 8)
- wall spread above 1.10x for ('framed_rpc', 4, 1, 1024, 32)
- CPU spread above 1.10x for ('framed_rpc', 4, 512, 1024, 1)
- wall spread above 1.10x for ('framed_rpc', 8, 1, 64, 1)
- CPU spread above 1.10x for ('framed_rpc', 8, 1, 64, 1)
- wall spread above 1.10x for ('framed_rpc', 8, 1, 64, 8)
- wall spread above 1.10x for ('framed_rpc', 8, 1, 64, 32)
- CPU spread above 1.10x for ('framed_rpc', 8, 1, 64, 32)
- wall spread above 1.10x for ('framed_rpc', 8, 1, 1024, 32)
- CPU spread above 1.10x for ('framed_rpc', 8, 512, 64, 32)
- CPU spread above 1.10x for ('framed_rpc', 8, 512, 1024, 1)
- wall spread above 1.10x for ('graph_break', 1, 1, 64, 8)
- wall spread above 1.10x for ('graph_break', 1, 1, 1024, 8)
- wall spread above 1.10x for ('socket_relay', 1, 1, 64, 1)
- CPU spread above 1.10x for ('socket_relay', 1, 1, 64, 1)
- wall spread above 1.10x for ('socket_relay', 1, 1, 64, 8)
- wall spread above 1.10x for ('socket_relay', 1, 1, 64, 32)
- wall spread above 1.10x for ('socket_relay', 1, 1, 1024, 1)
- CPU spread above 1.10x for ('socket_relay', 1, 1, 1024, 1)
- wall spread above 1.10x for ('socket_relay', 1, 1, 1024, 8)
- CPU spread above 1.10x for ('socket_relay', 1, 1, 1024, 8)
- wall spread above 1.10x for ('socket_relay', 1, 1, 1024, 32)
- CPU spread above 1.10x for ('socket_relay', 1, 1, 1024, 32)
- CPU spread above 1.10x for ('socket_relay', 1, 512, 64, 32)
- wall spread above 1.10x for ('socket_relay', 2, 1, 64, 1)
- wall spread above 1.10x for ('socket_relay', 2, 1, 64, 8)
- wall spread above 1.10x for ('socket_relay', 2, 1, 64, 32)
- wall spread above 1.10x for ('socket_relay', 2, 1, 1024, 1)
- CPU spread above 1.10x for ('socket_relay', 2, 1, 1024, 1)
- wall spread above 1.10x for ('socket_relay', 2, 1, 1024, 8)
- CPU spread above 1.10x for ('socket_relay', 2, 1, 1024, 8)
- wall spread above 1.10x for ('socket_relay', 2, 1, 1024, 32)
- CPU spread above 1.10x for ('socket_relay', 2, 1, 1024, 32)
- wall spread above 1.10x for ('socket_relay', 4, 1, 64, 1)
- CPU spread above 1.10x for ('socket_relay', 4, 1, 64, 1)
- wall spread above 1.10x for ('socket_relay', 4, 1, 64, 8)
- CPU spread above 1.10x for ('socket_relay', 4, 1, 64, 8)
- wall spread above 1.10x for ('socket_relay', 4, 1, 64, 32)
- wall spread above 1.10x for ('socket_relay', 4, 1, 1024, 1)
- wall spread above 1.10x for ('socket_relay', 4, 1, 1024, 8)
- CPU spread above 1.10x for ('socket_relay', 4, 1, 1024, 8)
- wall spread above 1.10x for ('socket_relay', 4, 1, 1024, 32)
- wall spread above 1.10x for ('socket_relay', 8, 1, 64, 1)
- CPU spread above 1.10x for ('socket_relay', 8, 1, 64, 1)
- wall spread above 1.10x for ('socket_relay', 8, 1, 1024, 1)
- CPU spread above 1.10x for ('socket_relay', 8, 1, 1024, 1)
- wall spread above 1.10x for ('socket_relay', 8, 1, 1024, 8)
- wall spread above 1.10x for ('socket_relay', 8, 1, 1024, 32)
- wall spread above 1.10x for ('socket_relay', 8, 512, 64, 8)
- CPU spread above 1.10x for ('socket_relay', 8, 512, 64, 8)

## Cell summaries

| workload | nodes | concurrency | payload | budget | samples | wall | CPU | wall spread | CPU spread | service p99 | terminal p99 | mechanism | CPU scope |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| framed_rpc | 1 | 1 | 64 | 1 | 5 | 0.887748x | 1.239016x | 1.173798x | 1.049587x | 1.003417x | 0.999338x | valid | server |
| framed_rpc | 1 | 1 | 64 | 8 | 5 | 0.859479x | 1.278008x | 1.264081x | 1.086663x | 0.998079x | 1.015772x | valid | server |
| framed_rpc | 1 | 1 | 64 | 32 | 5 | 0.859438x | 1.272319x | 1.210145x | 1.056946x | 0.998675x | 1.010653x | valid | server |
| framed_rpc | 1 | 1 | 1024 | 1 | 5 | 0.817022x | 1.192237x | 1.244618x | 1.146775x | 0.901471x | 1.016744x | valid | server |
| framed_rpc | 1 | 1 | 1024 | 8 | 5 | 0.860865x | 1.134990x | 1.221009x | 1.088918x | 0.997600x | 1.001024x | valid | server |
| framed_rpc | 1 | 1 | 1024 | 32 | 5 | 0.818311x | 1.163347x | 1.188303x | 1.103206x | 0.921517x | 1.016045x | valid | server |
| framed_rpc | 1 | 1 | 16384 | 1 | 5 | 0.629735x | 1.264734x | 1.013317x | 1.006069x | 0.160673x | 6.547564x | valid | server |
| framed_rpc | 1 | 1 | 16384 | 8 | 5 | 0.629833x | 1.268244x | 1.030429x | 1.012482x | 0.182240x | 6.653238x | valid | server |
| framed_rpc | 1 | 1 | 16384 | 32 | 5 | 0.636362x | 1.261058x | 1.049346x | 1.006940x | 0.182766x | 6.627711x | valid | server |
| framed_rpc | 1 | 64 | 64 | 1 | 5 | 0.927846x | 1.176275x | 1.023563x | 1.022437x | 0.571235x | 1.048710x | valid | server |
| framed_rpc | 1 | 64 | 64 | 8 | 5 | 0.931443x | 1.167726x | 1.011131x | 1.017037x | 0.615123x | 1.033668x | valid | server |
| framed_rpc | 1 | 64 | 64 | 32 | 5 | 0.928123x | 1.169437x | 1.028339x | 1.021687x | 0.534818x | 0.836854x | valid | server |
| framed_rpc | 1 | 64 | 1024 | 1 | 5 | 0.994517x | 1.067544x | 1.015044x | 1.015521x | 0.154286x | 0.360501x | valid | server |
| framed_rpc | 1 | 64 | 1024 | 8 | 5 | 0.992807x | 1.068123x | 1.025795x | 1.030583x | 0.154867x | 0.388849x | valid | server |
| framed_rpc | 1 | 64 | 1024 | 32 | 5 | 1.006877x | 1.049713x | 1.014402x | 1.022107x | 0.161735x | 0.370864x | valid | server |
| framed_rpc | 1 | 64 | 16384 | 1 | 5 | 1.002352x | 0.998784x | 1.010529x | 1.024927x | 0.734593x | 0.990055x | valid | server |
| framed_rpc | 1 | 64 | 16384 | 8 | 5 | 1.002516x | 0.995640x | 1.017244x | 1.009336x | 1.708997x | 1.495630x | valid | server |
| framed_rpc | 1 | 64 | 16384 | 32 | 5 | 1.000038x | 1.000067x | 1.012681x | 1.019890x | 2.086683x | 1.472604x | valid | server |
| framed_rpc | 1 | 512 | 64 | 1 | 5 | 0.912343x | 1.206817x | 1.041060x | 1.039127x | 0.123503x | 0.391637x | valid | server |
| framed_rpc | 1 | 512 | 64 | 8 | 5 | 0.897435x | 1.233835x | 1.047386x | 1.062337x | 0.098723x | 0.335435x | valid | server |
| framed_rpc | 1 | 512 | 64 | 32 | 5 | 0.905643x | 1.232548x | 1.040907x | 1.047079x | 0.103629x | 0.323094x | valid | server |
| framed_rpc | 1 | 512 | 1024 | 1 | 5 | 0.967274x | 1.077167x | 1.041146x | 1.097579x | 0.618736x | 0.420937x | valid | server |
| framed_rpc | 1 | 512 | 1024 | 8 | 5 | 0.968924x | 1.076175x | 1.024427x | 1.049525x | 0.819524x | 0.659662x | valid | server |
| framed_rpc | 1 | 512 | 1024 | 32 | 5 | 0.971938x | 1.072702x | 1.024635x | 1.033033x | 0.650657x | 0.462606x | valid | server |
| framed_rpc | 1 | 512 | 16384 | 1 | 5 | 1.003677x | 0.989732x | 1.014102x | 1.007165x | 0.996123x | 0.991931x | valid | server |
| framed_rpc | 1 | 512 | 16384 | 8 | 5 | 0.993970x | 0.990314x | 1.020245x | 1.011900x | 0.996290x | 1.006473x | valid | server |
| framed_rpc | 1 | 512 | 16384 | 32 | 5 | 0.999116x | 0.991114x | 1.009530x | 1.005874x | 0.999118x | 0.996287x | valid | server |
| framed_rpc | 2 | 1 | 64 | 1 | 5 | 0.091230x | 2.639111x | 1.149483x | 1.075199x | 0.919135x | 1.947144x | valid | server |
| framed_rpc | 2 | 1 | 64 | 8 | 5 | 0.093980x | 2.641329x | 1.093208x | 1.055369x | 0.939164x | 1.942261x | valid | server |
| framed_rpc | 2 | 1 | 64 | 32 | 5 | 0.089295x | 2.766037x | 1.058048x | 1.049323x | 0.922781x | 1.952295x | valid | server |
| framed_rpc | 2 | 1 | 1024 | 1 | 5 | 0.328417x | 2.029228x | 1.287551x | 1.086844x | 0.962198x | 1.932424x | valid | server |
| framed_rpc | 2 | 1 | 1024 | 8 | 5 | 0.328023x | 1.945424x | 1.285130x | 1.106859x | 0.966275x | 1.947196x | valid | server |
| framed_rpc | 2 | 1 | 1024 | 32 | 5 | 0.275236x | 2.059673x | 1.286923x | 1.090586x | 0.958787x | 1.937050x | valid | server |
| framed_rpc | 2 | 1 | 16384 | 1 | 5 | 0.144304x | 1.863471x | 1.017295x | 1.006638x | 0.157224x | 13.662430x | valid | server |
| framed_rpc | 2 | 1 | 16384 | 8 | 5 | 0.144045x | 1.862548x | 1.007335x | 1.009751x | 0.203252x | 13.496525x | valid | server |
| framed_rpc | 2 | 1 | 16384 | 32 | 5 | 0.144391x | 1.867992x | 1.030272x | 1.020992x | 0.180252x | 13.532126x | valid | server |
| framed_rpc | 2 | 64 | 64 | 1 | 5 | 0.560412x | 1.191455x | 1.032649x | 1.007562x | 1.215570x | 1.324978x | valid | server |
| framed_rpc | 2 | 64 | 64 | 8 | 5 | 0.556545x | 1.196680x | 1.041366x | 1.018823x | 1.274971x | 1.386178x | valid | server |
| framed_rpc | 2 | 64 | 64 | 32 | 5 | 0.561834x | 1.196589x | 1.054002x | 1.028417x | 1.283207x | 1.173966x | valid | server |
| framed_rpc | 2 | 64 | 1024 | 1 | 5 | 0.813753x | 1.219299x | 1.014064x | 1.032402x | 0.163129x | 0.434297x | valid | server |
| framed_rpc | 2 | 64 | 1024 | 8 | 5 | 0.819925x | 1.210541x | 1.055775x | 1.028363x | 0.196823x | 0.440884x | valid | server |
| framed_rpc | 2 | 64 | 1024 | 32 | 5 | 0.822686x | 1.225345x | 1.022984x | 1.029788x | 0.167379x | 0.443276x | valid | server |
| framed_rpc | 2 | 64 | 16384 | 1 | 5 | 0.826154x | 1.246707x | 1.024492x | 1.006052x | 6.731875x | 2.852249x | valid | server |
| framed_rpc | 2 | 64 | 16384 | 8 | 5 | 0.826695x | 1.248882x | 1.025246x | 1.011434x | 5.779548x | 3.092527x | valid | server |
| framed_rpc | 2 | 64 | 16384 | 32 | 5 | 0.813560x | 1.251261x | 1.020607x | 1.029523x | 6.591828x | 2.900794x | valid | server |
| framed_rpc | 2 | 512 | 64 | 1 | 5 | 0.805756x | 1.351276x | 1.071765x | 1.103475x | 0.191193x | 0.387438x | valid | server |
| framed_rpc | 2 | 512 | 64 | 8 | 5 | 0.813629x | 1.351988x | 1.052151x | 1.078585x | 0.187487x | 0.347520x | valid | server |
| framed_rpc | 2 | 512 | 64 | 32 | 5 | 0.796279x | 1.394887x | 1.106352x | 1.143302x | 0.188479x | 0.359373x | valid | server |
| framed_rpc | 2 | 512 | 1024 | 1 | 5 | 0.928720x | 1.176255x | 1.035137x | 1.058352x | 0.637400x | 0.572425x | valid | server |
| framed_rpc | 2 | 512 | 1024 | 8 | 5 | 0.910758x | 1.221547x | 1.045898x | 1.084419x | 0.798639x | 0.658856x | valid | server |
| framed_rpc | 2 | 512 | 1024 | 32 | 5 | 0.933632x | 1.172611x | 1.034361x | 1.098226x | 0.648162x | 0.586525x | valid | server |
| framed_rpc | 2 | 512 | 16384 | 1 | 5 | 0.956426x | 1.226105x | 1.024295x | 1.011120x | 1.052218x | 1.057243x | valid | server |
| framed_rpc | 2 | 512 | 16384 | 8 | 5 | 0.948929x | 1.231090x | 1.015590x | 1.006686x | 0.974880x | 1.056841x | valid | server |
| framed_rpc | 2 | 512 | 16384 | 32 | 5 | 0.946777x | 1.233413x | 1.022406x | 1.011141x | 1.010179x | 1.070794x | valid | server |
| framed_rpc | 4 | 1 | 64 | 1 | 5 | 0.067620x | 2.815686x | 1.187814x | 1.086883x | 0.935640x | 3.730035x | valid | server |
| framed_rpc | 4 | 1 | 64 | 8 | 5 | 0.066941x | 2.794890x | 1.083160x | 1.077433x | 0.937596x | 3.731258x | valid | server |
| framed_rpc | 4 | 1 | 64 | 32 | 5 | 0.066922x | 2.808446x | 1.068346x | 1.059996x | 0.947218x | 3.687153x | valid | server |
| framed_rpc | 4 | 1 | 1024 | 1 | 5 | 0.177276x | 2.292322x | 1.073834x | 1.011094x | 0.864004x | 3.677222x | valid | server |
| framed_rpc | 4 | 1 | 1024 | 8 | 5 | 0.189297x | 2.260331x | 1.138404x | 1.065904x | 0.935688x | 3.627711x | valid | server |
| framed_rpc | 4 | 1 | 1024 | 32 | 5 | 0.183767x | 2.275112x | 1.187666x | 1.046637x | 0.858884x | 2.064006x | valid | server |
| framed_rpc | 4 | 1 | 16384 | 1 | 5 | 0.065556x | 1.963544x | 1.012141x | 1.008099x | 0.171560x | 13.826697x | valid | server |
| framed_rpc | 4 | 1 | 16384 | 8 | 5 | 0.065227x | 1.964043x | 1.012318x | 1.009644x | 0.168827x | 14.937684x | valid | server |
| framed_rpc | 4 | 1 | 16384 | 32 | 5 | 0.065384x | 1.953506x | 1.008928x | 1.015192x | 0.175988x | 15.270978x | valid | server |
| framed_rpc | 4 | 64 | 64 | 1 | 5 | 0.557516x | 1.266413x | 1.046388x | 1.017245x | 1.223184x | 1.622985x | valid | server |
| framed_rpc | 4 | 64 | 64 | 8 | 5 | 0.555947x | 1.273872x | 1.036976x | 1.018620x | 1.236072x | 1.771467x | valid | server |
| framed_rpc | 4 | 64 | 64 | 32 | 5 | 0.557924x | 1.262332x | 1.026560x | 1.018354x | 1.234658x | 1.710994x | valid | server |
| framed_rpc | 4 | 64 | 1024 | 1 | 5 | 0.801651x | 1.099214x | 1.026432x | 1.015154x | 0.229335x | 0.496511x | valid | server |
| framed_rpc | 4 | 64 | 1024 | 8 | 5 | 0.799400x | 1.101727x | 1.018344x | 1.026607x | 0.238572x | 0.560163x | valid | server |
| framed_rpc | 4 | 64 | 1024 | 32 | 5 | 0.789771x | 1.103468x | 1.055224x | 1.068559x | 0.232597x | 0.537571x | valid | server |
| framed_rpc | 4 | 64 | 16384 | 1 | 5 | 0.867385x | 1.079637x | 1.091888x | 1.004316x | 1.905495x | 2.143202x | valid | server |
| framed_rpc | 4 | 64 | 16384 | 8 | 5 | 0.863898x | 1.082845x | 1.060044x | 1.023829x | 1.718217x | 1.591153x | valid | server |
| framed_rpc | 4 | 64 | 16384 | 32 | 5 | 0.870482x | 1.077893x | 1.043490x | 1.039625x | 1.507732x | 2.024091x | valid | server |
| framed_rpc | 4 | 512 | 64 | 1 | 5 | 0.805182x | 1.345630x | 1.005835x | 1.008384x | 0.140922x | 0.454478x | valid | server |
| framed_rpc | 4 | 512 | 64 | 8 | 5 | 0.795167x | 1.374077x | 1.060429x | 1.098729x | 0.168113x | 0.414669x | valid | server |
| framed_rpc | 4 | 512 | 64 | 32 | 5 | 0.805184x | 1.352311x | 1.033059x | 1.060004x | 0.132870x | 0.437957x | valid | server |
| framed_rpc | 4 | 512 | 1024 | 1 | 5 | 0.974734x | 1.067009x | 1.056252x | 1.137185x | 0.412229x | 0.591296x | valid | server |
| framed_rpc | 4 | 512 | 1024 | 8 | 5 | 0.960974x | 1.113207x | 1.051577x | 1.073917x | 0.364698x | 0.510845x | valid | server |
| framed_rpc | 4 | 512 | 1024 | 32 | 5 | 0.975042x | 1.082915x | 1.043643x | 1.081844x | 0.395434x | 0.542011x | valid | server |
| framed_rpc | 4 | 512 | 16384 | 1 | 5 | 0.987309x | 1.081467x | 1.018268x | 1.012319x | 2.221264x | 1.010966x | valid | server |
| framed_rpc | 4 | 512 | 16384 | 8 | 5 | 0.987898x | 1.083830x | 1.010595x | 1.006630x | 0.834175x | 1.020022x | valid | server |
| framed_rpc | 4 | 512 | 16384 | 32 | 5 | 0.985563x | 1.079060x | 1.017610x | 1.009396x | 0.873658x | 1.011548x | valid | server |
| framed_rpc | 8 | 1 | 64 | 1 | 5 | 0.053058x | 2.981337x | 1.262950x | 1.113758x | 0.937259x | 3.886891x | valid | server |
| framed_rpc | 8 | 1 | 64 | 8 | 5 | 0.055607x | 2.930652x | 1.119154x | 1.083195x | 0.926040x | 3.898059x | valid | server |
| framed_rpc | 8 | 1 | 64 | 32 | 5 | 0.055587x | 2.906900x | 1.183566x | 1.120057x | 0.918503x | 3.891378x | valid | server |
| framed_rpc | 8 | 1 | 1024 | 1 | 5 | 0.138332x | 2.439167x | 1.056658x | 1.036602x | 0.946994x | 3.483499x | valid | server |
| framed_rpc | 8 | 1 | 1024 | 8 | 5 | 0.138863x | 2.416392x | 1.053131x | 1.041717x | 0.954680x | 3.839566x | valid | server |
| framed_rpc | 8 | 1 | 1024 | 32 | 5 | 0.137538x | 2.443968x | 1.289283x | 1.096566x | 0.946766x | 3.827588x | valid | server |
| framed_rpc | 8 | 1 | 16384 | 1 | 5 | 0.051564x | 1.977185x | 1.007157x | 1.030806x | 0.183513x | 16.476071x | valid | server |
| framed_rpc | 8 | 1 | 16384 | 8 | 5 | 0.051484x | 1.984769x | 1.010809x | 1.022853x | 0.185234x | 16.142282x | valid | server |
| framed_rpc | 8 | 1 | 16384 | 32 | 5 | 0.051481x | 1.989704x | 1.002545x | 1.006247x | 0.197590x | 15.599969x | valid | server |
| framed_rpc | 8 | 64 | 64 | 1 | 5 | 0.721784x | 1.218648x | 1.024468x | 1.023888x | 1.230450x | 2.064464x | valid | server |
| framed_rpc | 8 | 64 | 64 | 8 | 5 | 0.714943x | 1.239149x | 1.046957x | 1.023857x | 1.219927x | 2.202804x | valid | server |
| framed_rpc | 8 | 64 | 64 | 32 | 5 | 0.718732x | 1.228442x | 1.036401x | 1.014686x | 1.255448x | 2.140603x | valid | server |
| framed_rpc | 8 | 64 | 1024 | 1 | 5 | 0.849609x | 1.049244x | 1.056023x | 1.050436x | 0.214641x | 0.745214x | valid | server |
| framed_rpc | 8 | 64 | 1024 | 8 | 5 | 0.870677x | 1.061203x | 1.022995x | 1.013432x | 0.207578x | 0.731883x | valid | server |
| framed_rpc | 8 | 64 | 1024 | 32 | 5 | 0.869190x | 1.055319x | 1.048219x | 1.014486x | 0.186183x | 0.744325x | valid | server |
| framed_rpc | 8 | 64 | 16384 | 1 | 5 | 0.939899x | 0.970135x | 1.049415x | 1.034329x | 4.250379x | 1.479370x | valid | server |
| framed_rpc | 8 | 64 | 16384 | 8 | 5 | 0.927010x | 0.973351x | 1.095430x | 1.027432x | 4.936222x | 1.318162x | valid | server |
| framed_rpc | 8 | 64 | 16384 | 32 | 5 | 0.959902x | 0.971530x | 1.071402x | 1.041593x | 4.541479x | 1.394638x | valid | server |
| framed_rpc | 8 | 512 | 64 | 1 | 5 | 0.816295x | 1.294768x | 1.031019x | 1.054552x | 0.122056x | 0.397995x | valid | server |
| framed_rpc | 8 | 512 | 64 | 8 | 5 | 0.810249x | 1.313800x | 1.057940x | 1.092606x | 0.125200x | 0.403756x | valid | server |
| framed_rpc | 8 | 512 | 64 | 32 | 5 | 0.832642x | 1.281687x | 1.068051x | 1.105850x | 0.138867x | 0.476359x | valid | server |
| framed_rpc | 8 | 512 | 1024 | 1 | 5 | 1.020823x | 0.976932x | 1.047162x | 1.101505x | 1.095394x | 0.761962x | valid | server |
| framed_rpc | 8 | 512 | 1024 | 8 | 5 | 1.022504x | 0.971636x | 1.029338x | 1.043173x | 0.881179x | 0.762481x | valid | server |
| framed_rpc | 8 | 512 | 1024 | 32 | 5 | 1.011527x | 0.991884x | 1.023272x | 1.040213x | 0.665131x | 0.765526x | valid | server |
| framed_rpc | 8 | 512 | 16384 | 1 | 5 | 1.013412x | 0.991215x | 1.005236x | 1.012727x | 0.420098x | 0.980328x | valid | server |
| framed_rpc | 8 | 512 | 16384 | 8 | 5 | 1.010726x | 0.990620x | 1.025032x | 1.022644x | 0.317392x | 0.987935x | valid | server |
| framed_rpc | 8 | 512 | 16384 | 32 | 5 | 1.009725x | 0.988548x | 1.009705x | 1.017406x | 0.401893x | 0.991751x | valid | server |
| graph_break | 1 | 1 | 64 | 8 | 5 | 0.833723x | 1.275918x | 1.280894x | 1.077832x | 1.004768x | 1.020846x | valid | server |
| graph_break | 1 | 1 | 1024 | 8 | 5 | 0.885662x | 1.139143x | 1.281233x | 1.091804x | 0.961622x | 1.018237x | valid | server |
| graph_break | 1 | 1 | 16384 | 8 | 5 | 0.629330x | 1.270998x | 1.042835x | 1.009882x | 0.155887x | 6.665466x | valid | server |
| graph_break | 1 | 64 | 64 | 8 | 5 | 0.927627x | 1.174715x | 1.014782x | 1.024364x | 0.590953x | 1.301833x | valid | server |
| graph_break | 1 | 64 | 1024 | 8 | 5 | 0.998556x | 1.058305x | 1.021797x | 1.023397x | 0.149616x | 0.372781x | valid | server |
| graph_break | 1 | 64 | 16384 | 8 | 5 | 1.000816x | 0.995664x | 1.017964x | 1.015869x | 1.667067x | 1.304727x | valid | server |
| graph_break | 1 | 512 | 64 | 8 | 5 | 0.907162x | 1.223378x | 1.045858x | 1.068444x | 0.116734x | 0.373425x | valid | server |
| graph_break | 1 | 512 | 1024 | 8 | 5 | 0.970818x | 1.069350x | 1.031390x | 1.068625x | 0.637138x | 0.481269x | valid | server |
| graph_break | 1 | 512 | 16384 | 8 | 5 | 1.004815x | 0.987740x | 1.014132x | 1.008649x | 1.000439x | 0.996732x | valid | server |
| socket_relay | 1 | 1 | 64 | 1 | 5 | 0.920771x | 1.228588x | 1.334525x | 1.118410x | 1.000387x | 1.008389x | valid | server |
| socket_relay | 1 | 1 | 64 | 8 | 5 | 0.928217x | 1.227381x | 1.226662x | 1.069130x | 1.002552x | 1.006814x | valid | server |
| socket_relay | 1 | 1 | 64 | 32 | 5 | 0.929568x | 1.224817x | 1.169919x | 1.060457x | 1.001666x | 0.999935x | valid | server |
| socket_relay | 1 | 1 | 1024 | 1 | 5 | 0.886431x | 1.113433x | 1.302908x | 1.140351x | 1.020964x | 1.013355x | valid | server |
| socket_relay | 1 | 1 | 1024 | 8 | 5 | 0.914466x | 1.139340x | 1.281692x | 1.140849x | 0.980297x | 1.002500x | valid | server |
| socket_relay | 1 | 1 | 1024 | 32 | 5 | 0.898651x | 1.134899x | 1.225697x | 1.105658x | 1.011545x | 1.006202x | valid | server |
| socket_relay | 1 | 1 | 16384 | 1 | 5 | 0.622503x | 1.269259x | 1.028737x | 1.011476x | 0.179335x | 6.822377x | valid | server |
| socket_relay | 1 | 1 | 16384 | 8 | 5 | 0.631578x | 1.272308x | 1.033913x | 1.027163x | 0.178957x | 6.638486x | valid | server |
| socket_relay | 1 | 1 | 16384 | 32 | 5 | 0.635307x | 1.266605x | 1.045811x | 1.005383x | 0.158256x | 6.630397x | valid | server |
| socket_relay | 1 | 64 | 64 | 1 | 5 | 0.922917x | 1.170730x | 1.011703x | 1.017831x | 0.585537x | 1.106409x | valid | server |
| socket_relay | 1 | 64 | 64 | 8 | 5 | 0.928296x | 1.168222x | 1.013999x | 1.007796x | 0.608052x | 1.285935x | valid | server |
| socket_relay | 1 | 64 | 64 | 32 | 5 | 0.924318x | 1.167865x | 1.008512x | 1.022137x | 0.575564x | 1.124553x | valid | server |
| socket_relay | 1 | 64 | 1024 | 1 | 5 | 0.995854x | 1.068617x | 1.012847x | 1.040922x | 0.141680x | 0.362356x | valid | server |
| socket_relay | 1 | 64 | 1024 | 8 | 5 | 1.002477x | 1.059039x | 1.036433x | 1.025881x | 0.158633x | 0.372211x | valid | server |
| socket_relay | 1 | 64 | 1024 | 32 | 5 | 1.006313x | 1.054239x | 1.012596x | 1.016367x | 0.151139x | 0.349357x | valid | server |
| socket_relay | 1 | 64 | 16384 | 1 | 5 | 1.001396x | 0.996150x | 1.012586x | 1.013442x | 1.217505x | 1.276188x | valid | server |
| socket_relay | 1 | 64 | 16384 | 8 | 5 | 1.002669x | 0.992809x | 1.025995x | 1.024456x | 1.465319x | 1.188036x | valid | server |
| socket_relay | 1 | 64 | 16384 | 32 | 5 | 1.007799x | 0.994481x | 1.016868x | 1.011911x | 1.458962x | 1.290658x | valid | server |
| socket_relay | 1 | 512 | 64 | 1 | 5 | 0.895440x | 1.228180x | 1.038291x | 1.032114x | 0.108578x | 0.392191x | valid | server |
| socket_relay | 1 | 512 | 64 | 8 | 5 | 0.902888x | 1.202899x | 1.061830x | 1.085514x | 0.153689x | 0.383115x | valid | server |
| socket_relay | 1 | 512 | 64 | 32 | 5 | 0.912844x | 1.213300x | 1.072728x | 1.109622x | 0.138682x | 0.354446x | valid | server |
| socket_relay | 1 | 512 | 1024 | 1 | 5 | 0.973464x | 1.051901x | 1.045237x | 1.082982x | 0.831007x | 0.529222x | valid | server |
| socket_relay | 1 | 512 | 1024 | 8 | 5 | 0.976064x | 1.061507x | 1.048043x | 1.055192x | 0.960124x | 0.892820x | valid | server |
| socket_relay | 1 | 512 | 1024 | 32 | 5 | 0.963185x | 1.082451x | 1.028192x | 1.061367x | 0.805753x | 0.571766x | valid | server |
| socket_relay | 1 | 512 | 16384 | 1 | 5 | 1.001215x | 0.989284x | 1.008786x | 1.008716x | 0.998758x | 0.995186x | valid | server |
| socket_relay | 1 | 512 | 16384 | 8 | 5 | 1.003726x | 0.993080x | 1.036034x | 1.021428x | 0.988004x | 1.002142x | valid | server |
| socket_relay | 1 | 512 | 16384 | 32 | 5 | 1.000189x | 0.991728x | 1.014039x | 1.015245x | 1.035685x | 0.997925x | valid | server |
| socket_relay | 2 | 1 | 64 | 1 | 5 | 0.092151x | 2.653100x | 1.161276x | 1.081140x | 0.922792x | 1.950896x | valid | server |
| socket_relay | 2 | 1 | 64 | 8 | 5 | 0.089183x | 2.663389x | 1.127026x | 1.047879x | 0.935768x | 1.981684x | valid | server |
| socket_relay | 2 | 1 | 64 | 32 | 5 | 0.094828x | 2.610896x | 1.161537x | 1.031079x | 0.921892x | 1.947561x | valid | server |
| socket_relay | 2 | 1 | 1024 | 1 | 5 | 0.260248x | 2.004678x | 1.376302x | 1.127145x | 0.922866x | 1.938931x | valid | server |
| socket_relay | 2 | 1 | 1024 | 8 | 5 | 0.290227x | 2.064398x | 1.344967x | 1.102735x | 0.953896x | 1.955663x | valid | server |
| socket_relay | 2 | 1 | 1024 | 32 | 5 | 0.311464x | 1.997426x | 1.342493x | 1.115637x | 0.961520x | 1.920859x | valid | server |
| socket_relay | 2 | 1 | 16384 | 1 | 5 | 0.143722x | 1.867800x | 1.012472x | 1.006274x | 0.177092x | 13.473540x | valid | server |
| socket_relay | 2 | 1 | 16384 | 8 | 5 | 0.144083x | 1.870837x | 1.036602x | 1.010671x | 0.178621x | 13.606378x | valid | server |
| socket_relay | 2 | 1 | 16384 | 32 | 5 | 0.143167x | 1.871735x | 1.021838x | 1.012702x | 0.169394x | 13.576332x | valid | server |
| socket_relay | 2 | 64 | 64 | 1 | 5 | 0.556626x | 1.192526x | 1.027114x | 1.027847x | 1.263296x | 1.324749x | valid | server |
| socket_relay | 2 | 64 | 64 | 8 | 5 | 0.560236x | 1.192104x | 1.028574x | 1.020684x | 1.259876x | 1.249063x | valid | server |
| socket_relay | 2 | 64 | 64 | 32 | 5 | 0.553361x | 1.206064x | 1.017186x | 1.009428x | 1.282066x | 1.603210x | valid | server |
| socket_relay | 2 | 64 | 1024 | 1 | 5 | 0.820781x | 1.206305x | 1.033981x | 1.020576x | 0.182173x | 0.409714x | valid | server |
| socket_relay | 2 | 64 | 1024 | 8 | 5 | 0.818713x | 1.213666x | 1.037801x | 1.013461x | 0.170598x | 0.419618x | valid | server |
| socket_relay | 2 | 64 | 1024 | 32 | 5 | 0.815041x | 1.215190x | 1.037110x | 1.010756x | 0.161200x | 0.449209x | valid | server |
| socket_relay | 2 | 64 | 16384 | 1 | 5 | 0.826690x | 1.249756x | 1.044714x | 1.024403x | 5.631608x | 3.270843x | valid | server |
| socket_relay | 2 | 64 | 16384 | 8 | 5 | 0.821239x | 1.252981x | 1.026492x | 1.019135x | 4.745803x | 2.746551x | valid | server |
| socket_relay | 2 | 64 | 16384 | 32 | 5 | 0.826758x | 1.252017x | 1.011522x | 1.009556x | 4.721945x | 3.260461x | valid | server |
| socket_relay | 2 | 512 | 64 | 1 | 5 | 0.806878x | 1.341097x | 1.041027x | 1.075112x | 0.154326x | 0.333477x | valid | server |
| socket_relay | 2 | 512 | 64 | 8 | 5 | 0.801976x | 1.363505x | 1.032960x | 1.036342x | 0.206394x | 0.401493x | valid | server |
| socket_relay | 2 | 512 | 64 | 32 | 5 | 0.806979x | 1.340966x | 1.049801x | 1.063822x | 0.192783x | 0.387601x | valid | server |
| socket_relay | 2 | 512 | 1024 | 1 | 5 | 0.917072x | 1.196954x | 1.038451x | 1.043824x | 0.477185x | 0.491254x | valid | server |
| socket_relay | 2 | 512 | 1024 | 8 | 5 | 0.917560x | 1.206120x | 1.029345x | 1.039032x | 0.851247x | 0.596449x | valid | server |
| socket_relay | 2 | 512 | 1024 | 32 | 5 | 0.922532x | 1.201991x | 1.027952x | 1.066482x | 0.657289x | 0.603898x | valid | server |
| socket_relay | 2 | 512 | 16384 | 1 | 5 | 0.956803x | 1.233069x | 1.017729x | 1.012965x | 1.018836x | 1.049261x | valid | server |
| socket_relay | 2 | 512 | 16384 | 8 | 5 | 0.945118x | 1.229859x | 1.019957x | 1.018668x | 1.024061x | 1.059055x | valid | server |
| socket_relay | 2 | 512 | 16384 | 32 | 5 | 0.953355x | 1.233666x | 1.020854x | 1.013555x | 1.006546x | 1.062668x | valid | server |
| socket_relay | 4 | 1 | 64 | 1 | 5 | 0.063938x | 2.912109x | 1.155295x | 1.122589x | 0.928029x | 3.701518x | valid | server |
| socket_relay | 4 | 1 | 64 | 8 | 5 | 0.064492x | 2.881734x | 1.357511x | 1.102709x | 0.943138x | 3.736675x | valid | server |
| socket_relay | 4 | 1 | 64 | 32 | 5 | 0.064649x | 2.837907x | 1.104727x | 1.050573x | 0.943963x | 3.682854x | valid | server |
| socket_relay | 4 | 1 | 1024 | 1 | 5 | 0.177532x | 2.326469x | 1.145844x | 1.064397x | 0.936412x | 3.673151x | valid | server |
| socket_relay | 4 | 1 | 1024 | 8 | 5 | 0.171249x | 2.271490x | 1.217095x | 1.102194x | 0.774938x | 3.437899x | valid | server |
| socket_relay | 4 | 1 | 1024 | 32 | 5 | 0.182159x | 2.285618x | 1.102232x | 1.032342x | 0.926051x | 3.690205x | valid | server |
| socket_relay | 4 | 1 | 16384 | 1 | 5 | 0.065496x | 1.967690x | 1.015368x | 1.016391x | 0.168902x | 14.830146x | valid | server |
| socket_relay | 4 | 1 | 16384 | 8 | 5 | 0.065376x | 1.977167x | 1.021016x | 1.020693x | 0.185330x | 14.802281x | valid | server |
| socket_relay | 4 | 1 | 16384 | 32 | 5 | 0.065444x | 1.972695x | 1.012877x | 1.016953x | 0.207416x | 14.624999x | valid | server |
| socket_relay | 4 | 64 | 64 | 1 | 5 | 0.549411x | 1.259052x | 1.047403x | 1.022915x | 1.231084x | 1.698095x | valid | server |
| socket_relay | 4 | 64 | 64 | 8 | 5 | 0.546414x | 1.263737x | 1.023535x | 1.024628x | 1.235600x | 1.720134x | valid | server |
| socket_relay | 4 | 64 | 64 | 32 | 5 | 0.553391x | 1.266192x | 1.033677x | 1.028997x | 1.244079x | 1.648006x | valid | server |
| socket_relay | 4 | 64 | 1024 | 1 | 5 | 0.796095x | 1.103995x | 1.018971x | 1.024017x | 0.230818x | 0.521899x | valid | server |
| socket_relay | 4 | 64 | 1024 | 8 | 5 | 0.793570x | 1.109638x | 1.052480x | 1.019521x | 0.256565x | 0.483905x | valid | server |
| socket_relay | 4 | 64 | 1024 | 32 | 5 | 0.779791x | 1.103442x | 1.037446x | 1.006609x | 0.249463x | 0.521693x | valid | server |
| socket_relay | 4 | 64 | 16384 | 1 | 5 | 0.881305x | 1.074159x | 1.071836x | 1.013346x | 1.596825x | 2.206603x | valid | server |
| socket_relay | 4 | 64 | 16384 | 8 | 5 | 0.863643x | 1.076193x | 1.039414x | 1.017641x | 2.182054x | 2.200944x | valid | server |
| socket_relay | 4 | 64 | 16384 | 32 | 5 | 0.881863x | 1.071622x | 1.092416x | 1.023638x | 2.612940x | 2.204083x | valid | server |
| socket_relay | 4 | 512 | 64 | 1 | 5 | 0.802758x | 1.356393x | 1.019284x | 1.038011x | 0.137313x | 0.380902x | valid | server |
| socket_relay | 4 | 512 | 64 | 8 | 5 | 0.794858x | 1.363331x | 1.037127x | 1.085966x | 0.130933x | 0.416078x | valid | server |
| socket_relay | 4 | 512 | 64 | 32 | 5 | 0.806825x | 1.343912x | 1.028663x | 1.049672x | 0.127644x | 0.396454x | valid | server |
| socket_relay | 4 | 512 | 1024 | 1 | 5 | 0.966924x | 1.095122x | 1.024998x | 1.053431x | 0.379351x | 0.482570x | valid | server |
| socket_relay | 4 | 512 | 1024 | 8 | 5 | 0.963352x | 1.108079x | 1.041206x | 1.081538x | 0.369422x | 0.458586x | valid | server |
| socket_relay | 4 | 512 | 1024 | 32 | 5 | 0.959257x | 1.113396x | 1.025269x | 1.049511x | 0.346777x | 0.544890x | valid | server |
| socket_relay | 4 | 512 | 16384 | 1 | 5 | 0.986863x | 1.083206x | 1.017754x | 1.005154x | 2.158087x | 1.025371x | valid | server |
| socket_relay | 4 | 512 | 16384 | 8 | 5 | 0.990530x | 1.077027x | 1.011771x | 1.010692x | 3.256807x | 1.013808x | valid | server |
| socket_relay | 4 | 512 | 16384 | 32 | 5 | 0.989470x | 1.079793x | 1.012502x | 1.005039x | 0.790468x | 1.011550x | valid | server |
| socket_relay | 8 | 1 | 64 | 1 | 5 | 0.057126x | 2.944284x | 1.224356x | 1.114946x | 0.931493x | 3.882471x | valid | server |
| socket_relay | 8 | 1 | 64 | 8 | 5 | 0.054891x | 3.067413x | 1.084159x | 1.049557x | 0.943591x | 3.899179x | valid | server |
| socket_relay | 8 | 1 | 64 | 32 | 5 | 0.056350x | 2.919200x | 1.062034x | 1.044715x | 0.943064x | 3.917979x | valid | server |
| socket_relay | 8 | 1 | 1024 | 1 | 5 | 0.139147x | 2.389175x | 1.444603x | 1.121117x | 0.936951x | 3.889770x | valid | server |
| socket_relay | 8 | 1 | 1024 | 8 | 5 | 0.136657x | 2.461572x | 1.119320x | 1.041595x | 0.941716x | 3.920545x | valid | server |
| socket_relay | 8 | 1 | 1024 | 32 | 5 | 0.136915x | 2.455572x | 1.153584x | 1.071095x | 0.945585x | 3.501759x | valid | server |
| socket_relay | 8 | 1 | 16384 | 1 | 5 | 0.051551x | 1.992382x | 1.007282x | 1.013538x | 0.176473x | 17.486971x | valid | server |
| socket_relay | 8 | 1 | 16384 | 8 | 5 | 0.051395x | 1.999330x | 1.004549x | 1.017760x | 0.144329x | 16.033322x | valid | server |
| socket_relay | 8 | 1 | 16384 | 32 | 5 | 0.051318x | 1.998429x | 1.004352x | 1.008217x | 0.182609x | 17.050646x | valid | server |
| socket_relay | 8 | 64 | 64 | 1 | 5 | 0.729107x | 1.221424x | 1.072828x | 1.025946x | 1.256126x | 2.158973x | valid | server |
| socket_relay | 8 | 64 | 64 | 8 | 5 | 0.717453x | 1.231991x | 1.056423x | 1.012399x | 1.254273x | 2.286777x | valid | server |
| socket_relay | 8 | 64 | 64 | 32 | 5 | 0.710543x | 1.233929x | 1.042115x | 1.015589x | 1.205171x | 2.250430x | valid | server |
| socket_relay | 8 | 64 | 1024 | 1 | 5 | 0.860733x | 1.057053x | 1.056734x | 1.027293x | 0.208678x | 0.760380x | valid | server |
| socket_relay | 8 | 64 | 1024 | 8 | 5 | 0.859829x | 1.057224x | 1.042814x | 1.016469x | 0.218661x | 0.738191x | valid | server |
| socket_relay | 8 | 64 | 1024 | 32 | 5 | 0.856296x | 1.056285x | 1.019000x | 1.017231x | 0.183961x | 0.767131x | valid | server |
| socket_relay | 8 | 64 | 16384 | 1 | 5 | 0.957375x | 0.969330x | 1.076594x | 1.034598x | 1.968835x | 1.317442x | valid | server |
| socket_relay | 8 | 64 | 16384 | 8 | 5 | 0.954498x | 0.962773x | 1.056176x | 1.016946x | 5.098293x | 1.426309x | valid | server |
| socket_relay | 8 | 64 | 16384 | 32 | 5 | 0.946416x | 0.971294x | 1.061176x | 1.023446x | 3.291302x | 1.352010x | valid | server |
| socket_relay | 8 | 512 | 64 | 1 | 5 | 0.821211x | 1.284181x | 1.035583x | 1.042815x | 0.114383x | 0.437768x | valid | server |
| socket_relay | 8 | 512 | 64 | 8 | 5 | 0.878342x | 1.155270x | 1.115237x | 1.192464x | 0.181302x | 0.636714x | valid | server |
| socket_relay | 8 | 512 | 64 | 32 | 5 | 0.819822x | 1.291543x | 1.031395x | 1.046954x | 0.140403x | 0.419975x | valid | server |
| socket_relay | 8 | 512 | 1024 | 1 | 5 | 1.019212x | 0.978632x | 1.042646x | 1.083505x | 0.768820x | 0.738555x | valid | server |
| socket_relay | 8 | 512 | 1024 | 8 | 5 | 1.019696x | 0.973105x | 1.049400x | 1.064546x | 0.639719x | 0.776179x | valid | server |
| socket_relay | 8 | 512 | 1024 | 32 | 5 | 1.030603x | 0.959961x | 1.037993x | 1.050976x | 0.820120x | 0.793207x | valid | server |
| socket_relay | 8 | 512 | 16384 | 1 | 5 | 1.009115x | 0.998776x | 1.007006x | 1.021411x | 0.542084x | 0.993223x | valid | server |
| socket_relay | 8 | 512 | 16384 | 8 | 5 | 1.014408x | 0.995912x | 1.014436x | 1.015064x | 0.352703x | 0.988375x | valid | server |
| socket_relay | 8 | 512 | 16384 | 32 | 5 | 1.009697x | 0.996849x | 1.003602x | 1.013418x | 1.320041x | 0.993131x | valid | server |

All ratios are medians of fresh-process native pairs. Spread is the retained maximum paired ratio divided by the retained minimum; no outlier is discarded.
