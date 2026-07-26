# SREM Phase 0 Results

- Phase: `screen`
- Verdict: `INCONCLUSIVE`

## Reasons

- screen summary failed integrity controls

## Summary

| workload | candidate | width | active | frame | sites | div/8 | threshold | wall | CPU | wall spread | CPU spread | fairness |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| srem_http_pipeline | adaptive_srem | 16 | 1 | 128 | 1 | 0 | 16 | 0.581223x | 1.720455x | 1.027591x | 1.027706x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 16 | 1 | 128 | 1 | 0 | 4 | 0.574237x | 1.741860x | 1.032122x | 1.039584x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 16 | 1 | 128 | 1 | 0 | 8 | 0.582281x | 1.717343x | 1.032930x | 1.023522x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 16 | 16 | 128 | 1 | 0 | 16 | 0.715961x | 1.396767x | 1.038806x | 1.039162x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 16 | 16 | 128 | 1 | 0 | 4 | 0.715220x | 1.398358x | 1.014748x | 1.014563x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 16 | 16 | 128 | 1 | 0 | 8 | 0.707663x | 1.412890x | 1.007818x | 1.008173x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 16 | 4 | 128 | 1 | 0 | 16 | 0.700423x | 1.427904x | 1.046366x | 1.046325x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 16 | 4 | 128 | 1 | 0 | 4 | 0.425336x | 2.350898x | 1.023732x | 1.023713x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 16 | 4 | 128 | 1 | 0 | 8 | 0.696035x | 1.437135x | 1.016350x | 1.015832x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 16 | 8 | 128 | 1 | 0 | 16 | 0.624627x | 1.601019x | 1.018753x | 1.018978x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 16 | 8 | 128 | 1 | 0 | 4 | 0.567536x | 1.761797x | 1.028909x | 1.021290x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 16 | 8 | 128 | 1 | 0 | 8 | 0.566227x | 1.765931x | 1.133647x | 1.133566x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 32 | 1 | 128 | 1 | 0 | 16 | 0.427217x | 2.341020x | 1.063790x | 1.063395x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 32 | 1 | 128 | 1 | 0 | 32 | 0.435269x | 2.297490x | 1.110652x | 1.110263x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 32 | 1 | 128 | 1 | 0 | 8 | 0.421875x | 2.371102x | 1.036365x | 1.036466x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 32 | 16 | 128 | 1 | 0 | 16 | 0.552455x | 1.810486x | 1.019961x | 1.019990x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 32 | 16 | 128 | 1 | 0 | 32 | 0.598301x | 1.671245x | 1.057941x | 1.057631x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 32 | 16 | 128 | 1 | 0 | 8 | 0.551531x | 1.812860x | 1.021822x | 1.021609x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 32 | 32 | 128 | 1 | 0 | 16 | 0.733104x | 1.363942x | 1.021174x | 1.020217x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 32 | 32 | 128 | 1 | 0 | 32 | 0.725976x | 1.377077x | 1.017519x | 1.013604x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 32 | 32 | 128 | 1 | 0 | 8 | 0.728486x | 1.372756x | 1.006819x | 1.009967x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 32 | 8 | 128 | 1 | 0 | 16 | 0.595503x | 1.679094x | 1.024241x | 1.024263x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 32 | 8 | 128 | 1 | 0 | 32 | 0.590325x | 1.694185x | 1.026378x | 1.033225x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 32 | 8 | 128 | 1 | 0 | 8 | 0.391927x | 2.551587x | 1.042225x | 1.042010x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 8 | 1 | 128 | 1 | 0 | 2 | 0.781045x | 1.280452x | 1.114029x | 1.113529x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 8 | 1 | 128 | 1 | 0 | 4 | 0.786537x | 1.271073x | 1.042848x | 1.043086x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 8 | 1 | 128 | 1 | 0 | 8 | 0.787928x | 1.269132x | 1.044400x | 1.044133x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 8 | 2 | 128 | 1 | 0 | 2 | 0.473135x | 2.114103x | 1.024510x | 1.024302x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 8 | 2 | 128 | 1 | 0 | 4 | 0.769244x | 1.300111x | 1.028329x | 1.028480x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 8 | 2 | 128 | 1 | 0 | 8 | 0.784755x | 1.274208x | 1.035321x | 1.035569x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 8 | 4 | 128 | 1 | 0 | 2 | 0.581859x | 1.718590x | 1.062396x | 1.062636x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 8 | 4 | 128 | 1 | 0 | 4 | 0.582376x | 1.717025x | 1.032106x | 1.032299x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 8 | 4 | 128 | 1 | 0 | 8 | 0.647723x | 1.543941x | 1.027116x | 1.027472x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 8 | 8 | 128 | 1 | 0 | 2 | 0.685078x | 1.459718x | 1.032817x | 1.032818x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 8 | 8 | 128 | 1 | 0 | 4 | 0.683496x | 1.463329x | 1.047046x | 1.046961x | 1.000000x |
| srem_http_pipeline | adaptive_srem | 8 | 8 | 128 | 1 | 0 | 8 | 0.690414x | 1.448531x | 1.031150x | 1.031413x | 1.000000x |
| srem_http_pipeline | tile_scalar | 16 | 1 | 128 | 1 | 0 | 8 | 0.574899x | 1.739388x | 1.026034x | 1.026081x | 1.000000x |
| srem_http_pipeline | tile_scalar | 16 | 16 | 128 | 1 | 0 | 8 | 0.566222x | 1.752591x | 1.014636x | 1.014650x | 1.000000x |
| srem_http_pipeline | tile_scalar | 16 | 4 | 128 | 1 | 0 | 8 | 0.688019x | 1.453616x | 1.011283x | 1.011398x | 1.000000x |
| srem_http_pipeline | tile_scalar | 16 | 8 | 128 | 1 | 0 | 8 | 0.624302x | 1.602082x | 1.087014x | 1.087392x | 1.000000x |
| srem_http_pipeline | tile_scalar | 32 | 1 | 128 | 1 | 0 | 16 | 0.416177x | 2.403305x | 1.047870x | 1.048058x | 1.000000x |
| srem_http_pipeline | tile_scalar | 32 | 16 | 128 | 1 | 0 | 16 | 0.593037x | 1.686502x | 1.010195x | 1.010645x | 1.000000x |
| srem_http_pipeline | tile_scalar | 32 | 32 | 128 | 1 | 0 | 16 | 0.576864x | 1.733872x | 1.014946x | 1.014459x | 1.000000x |
| srem_http_pipeline | tile_scalar | 32 | 8 | 128 | 1 | 0 | 16 | 0.593439x | 1.685215x | 1.031467x | 1.031367x | 1.000000x |
| srem_http_pipeline | tile_scalar | 8 | 1 | 128 | 1 | 0 | 4 | 0.765423x | 1.306821x | 1.099182x | 1.098832x | 1.000000x |
| srem_http_pipeline | tile_scalar | 8 | 2 | 128 | 1 | 0 | 4 | 0.817252x | 1.223405x | 1.081679x | 1.081837x | 1.000000x |
| srem_http_pipeline | tile_scalar | 8 | 4 | 128 | 1 | 0 | 4 | 0.661081x | 1.512734x | 1.028742x | 1.028949x | 1.000000x |
| srem_http_pipeline | tile_scalar | 8 | 8 | 128 | 1 | 0 | 4 | 0.559541x | 1.787316x | 1.020660x | 1.020371x | 1.000000x |
| srem_http_pipeline | tile_vector | 16 | 1 | 128 | 1 | 0 | 8 | 0.175568x | 5.696607x | 1.027140x | 1.027126x | 1.000000x |
| srem_http_pipeline | tile_vector | 16 | 16 | 128 | 1 | 0 | 8 | 0.715767x | 1.397192x | 1.014587x | 1.014754x | 1.000000x |
| srem_http_pipeline | tile_vector | 16 | 4 | 128 | 1 | 0 | 8 | 0.425132x | 2.352746x | 1.041503x | 1.041340x | 1.000000x |
| srem_http_pipeline | tile_vector | 16 | 8 | 128 | 1 | 0 | 8 | 0.570653x | 1.752429x | 1.020060x | 1.019395x | 1.000000x |
| srem_http_pipeline | tile_vector | 32 | 1 | 128 | 1 | 0 | 16 | 0.085993x | 11.631847x | 1.104898x | 1.104287x | 1.000000x |
| srem_http_pipeline | tile_vector | 32 | 16 | 128 | 1 | 0 | 16 | 0.556336x | 1.797520x | 1.024967x | 1.024954x | 1.000000x |
| srem_http_pipeline | tile_vector | 32 | 32 | 128 | 1 | 0 | 16 | 0.744162x | 1.343807x | 1.021527x | 1.021265x | 1.000000x |
| srem_http_pipeline | tile_vector | 32 | 8 | 128 | 1 | 0 | 16 | 0.389741x | 2.566301x | 1.071211x | 1.075230x | 1.000000x |
| srem_http_pipeline | tile_vector | 8 | 1 | 128 | 1 | 0 | 4 | 0.330594x | 3.026048x | 1.069288x | 1.069164x | 1.000000x |
| srem_http_pipeline | tile_vector | 8 | 2 | 128 | 1 | 0 | 4 | 0.462561x | 2.162147x | 1.026412x | 1.026769x | 1.000000x |
| srem_http_pipeline | tile_vector | 8 | 4 | 128 | 1 | 0 | 4 | 0.587801x | 1.701386x | 1.015669x | 1.015475x | 1.000000x |
| srem_http_pipeline | tile_vector | 8 | 8 | 128 | 1 | 0 | 4 | 0.688955x | 1.451663x | 1.019581x | 1.019428x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 16 | 1 | 128 | 1 | 0 | 16 | 0.585860x | 1.707140x | 1.029100x | 1.029130x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 16 | 1 | 128 | 1 | 0 | 4 | 0.582756x | 1.716172x | 1.090390x | 1.090048x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 16 | 1 | 128 | 1 | 0 | 8 | 0.603503x | 1.657085x | 1.041275x | 1.041113x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 16 | 16 | 128 | 1 | 0 | 16 | 0.711490x | 1.405386x | 1.022431x | 1.022618x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 16 | 16 | 128 | 1 | 0 | 4 | 0.716779x | 1.395370x | 1.028118x | 1.028110x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 16 | 16 | 128 | 1 | 0 | 8 | 0.715691x | 1.397477x | 1.009295x | 1.009431x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 16 | 4 | 128 | 1 | 0 | 16 | 0.675889x | 1.479762x | 1.026563x | 1.026596x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 16 | 4 | 128 | 1 | 0 | 4 | 0.419286x | 2.385225x | 1.024492x | 1.024184x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 16 | 4 | 128 | 1 | 0 | 8 | 0.698275x | 1.432398x | 1.054356x | 1.054239x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 16 | 8 | 128 | 1 | 0 | 16 | 0.608587x | 1.642878x | 1.044617x | 1.044561x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 16 | 8 | 128 | 1 | 0 | 4 | 0.563402x | 1.774798x | 1.050673x | 1.050493x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 16 | 8 | 128 | 1 | 0 | 8 | 0.558903x | 1.789420x | 1.010365x | 1.010269x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 32 | 1 | 128 | 1 | 0 | 16 | 0.428818x | 2.332168x | 1.081191x | 1.080832x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 32 | 1 | 128 | 1 | 0 | 32 | 0.423131x | 2.363508x | 1.014561x | 1.014754x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 32 | 1 | 128 | 1 | 0 | 8 | 0.431869x | 2.316018x | 1.035009x | 1.035039x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 32 | 16 | 128 | 1 | 0 | 16 | 0.553283x | 1.807851x | 1.021541x | 1.021746x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 32 | 16 | 128 | 1 | 0 | 32 | 0.579384x | 1.725883x | 1.013397x | 1.013377x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 32 | 16 | 128 | 1 | 0 | 8 | 0.543123x | 1.841394x | 1.037303x | 1.037262x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 32 | 32 | 128 | 1 | 0 | 16 | 0.722856x | 1.383063x | 1.030260x | 1.030671x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 32 | 32 | 128 | 1 | 0 | 32 | 0.724083x | 1.381165x | 1.012261x | 1.012487x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 32 | 32 | 128 | 1 | 0 | 8 | 0.720335x | 1.388318x | 1.018758x | 1.018675x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 32 | 8 | 128 | 1 | 0 | 16 | 0.583609x | 1.713126x | 1.026405x | 1.026979x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 32 | 8 | 128 | 1 | 0 | 32 | 0.582289x | 1.717781x | 1.029357x | 1.029043x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 32 | 8 | 128 | 1 | 0 | 8 | 0.376215x | 2.658228x | 1.032431x | 1.032021x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 8 | 1 | 128 | 1 | 0 | 2 | 0.776889x | 1.286982x | 1.062191x | 1.062368x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 8 | 1 | 128 | 1 | 0 | 4 | 0.790045x | 1.265850x | 1.031682x | 1.032201x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 8 | 1 | 128 | 1 | 0 | 8 | 0.777842x | 1.285002x | 1.373689x | 1.372615x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 8 | 2 | 128 | 1 | 0 | 2 | 0.450203x | 2.221114x | 1.044488x | 1.040854x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 8 | 2 | 128 | 1 | 0 | 4 | 0.750290x | 1.332814x | 1.017301x | 1.017272x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 8 | 2 | 128 | 1 | 0 | 8 | 0.764517x | 1.307693x | 1.054037x | 1.054003x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 8 | 4 | 128 | 1 | 0 | 2 | 0.575908x | 1.736702x | 1.017568x | 1.017647x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 8 | 4 | 128 | 1 | 0 | 4 | 0.579232x | 1.726274x | 1.058513x | 1.058637x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 8 | 4 | 128 | 1 | 0 | 8 | 0.636126x | 1.572315x | 1.026007x | 1.025881x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 8 | 8 | 128 | 1 | 0 | 2 | 0.681546x | 1.467473x | 1.030228x | 1.030287x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 8 | 8 | 128 | 1 | 0 | 4 | 0.682472x | 1.465301x | 1.019890x | 1.019820x | 1.000000x |
| srem_rpc_pipeline | adaptive_srem | 8 | 8 | 128 | 1 | 0 | 8 | 0.687447x | 1.454974x | 1.021131x | 1.020984x | 1.000000x |
| srem_rpc_pipeline | tile_scalar | 16 | 1 | 128 | 1 | 0 | 8 | 0.577330x | 1.732734x | 1.016428x | 1.016125x | 1.000000x |
| srem_rpc_pipeline | tile_scalar | 16 | 16 | 128 | 1 | 0 | 8 | 0.567833x | 1.761369x | 1.025790x | 1.025336x | 1.000000x |
| srem_rpc_pipeline | tile_scalar | 16 | 4 | 128 | 1 | 0 | 8 | 0.679036x | 1.472420x | 1.017729x | 1.017783x | 1.000000x |
| srem_rpc_pipeline | tile_scalar | 16 | 8 | 128 | 1 | 0 | 8 | 0.615350x | 1.625334x | 1.040144x | 1.040385x | 1.000000x |
| srem_rpc_pipeline | tile_scalar | 32 | 1 | 128 | 1 | 0 | 16 | 0.416368x | 2.401785x | 1.041695x | 1.041617x | 1.000000x |
| srem_rpc_pipeline | tile_scalar | 32 | 16 | 128 | 1 | 0 | 16 | 0.584425x | 1.710969x | 1.017886x | 1.017955x | 1.000000x |
| srem_rpc_pipeline | tile_scalar | 32 | 32 | 128 | 1 | 0 | 16 | 0.569696x | 1.755855x | 1.014653x | 1.018934x | 1.000000x |
| srem_rpc_pipeline | tile_scalar | 32 | 8 | 128 | 1 | 0 | 16 | 0.586644x | 1.704613x | 1.006963x | 1.006698x | 1.000000x |
| srem_rpc_pipeline | tile_scalar | 8 | 1 | 128 | 1 | 0 | 4 | 0.810674x | 1.233523x | 1.028024x | 1.028050x | 1.000000x |
| srem_rpc_pipeline | tile_scalar | 8 | 2 | 128 | 1 | 0 | 4 | 0.749397x | 1.334503x | 1.015083x | 1.015012x | 1.000000x |
| srem_rpc_pipeline | tile_scalar | 8 | 4 | 128 | 1 | 0 | 4 | 0.639810x | 1.562906x | 1.021751x | 1.021977x | 1.000000x |
| srem_rpc_pipeline | tile_scalar | 8 | 8 | 128 | 1 | 0 | 4 | 0.548339x | 1.823653x | 1.025573x | 1.022177x | 1.000000x |
| srem_rpc_pipeline | tile_vector | 16 | 1 | 128 | 1 | 0 | 8 | 0.176541x | 5.664562x | 1.047959x | 1.045093x | 1.000000x |
| srem_rpc_pipeline | tile_vector | 16 | 16 | 128 | 1 | 0 | 8 | 0.716068x | 1.396555x | 1.021479x | 1.021450x | 1.000000x |
| srem_rpc_pipeline | tile_vector | 16 | 4 | 128 | 1 | 0 | 8 | 0.422060x | 2.369571x | 1.040952x | 1.040741x | 1.000000x |
| srem_rpc_pipeline | tile_vector | 16 | 8 | 128 | 1 | 0 | 8 | 0.560699x | 1.782940x | 1.016141x | 1.016203x | 1.000000x |
| srem_rpc_pipeline | tile_vector | 32 | 1 | 128 | 1 | 0 | 16 | 0.086788x | 11.523230x | 1.124547x | 1.124259x | 1.000000x |
| srem_rpc_pipeline | tile_vector | 32 | 16 | 128 | 1 | 0 | 16 | 0.555106x | 1.801429x | 1.011697x | 1.011802x | 1.000000x |
| srem_rpc_pipeline | tile_vector | 32 | 32 | 128 | 1 | 0 | 16 | 0.733120x | 1.364329x | 1.011917x | 1.012151x | 1.000000x |
| srem_rpc_pipeline | tile_vector | 32 | 8 | 128 | 1 | 0 | 16 | 0.387582x | 2.579990x | 1.042255x | 1.041950x | 1.000000x |
| srem_rpc_pipeline | tile_vector | 8 | 1 | 128 | 1 | 0 | 4 | 0.325527x | 3.071923x | 1.064548x | 1.064760x | 1.000000x |
| srem_rpc_pipeline | tile_vector | 8 | 2 | 128 | 1 | 0 | 4 | 0.459912x | 2.174579x | 1.026947x | 1.032014x | 1.000000x |
| srem_rpc_pipeline | tile_vector | 8 | 4 | 128 | 1 | 0 | 4 | 0.579826x | 1.724984x | 1.016404x | 1.016628x | 1.000000x |
| srem_rpc_pipeline | tile_vector | 8 | 8 | 128 | 1 | 0 | 4 | 0.685875x | 1.458043x | 1.021946x | 1.021910x | 1.000000x |

This is a standalone cost-model result, not production-runtime validation.
