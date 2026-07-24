# Resultados medidos

Gerado por `scripts/benchmark.sh`. Para refazer:

```
scripts/benchmark.sh --audio data/entrada/longo.wav --reps 5
```

## Ambiente

| item | valor |
|---|---|
| data | 2026-07-24 16:06 |
| host | christian |
| nucleos | 28 |
| kernel | Linux 6.8.0-134-generic |
| compilador | gcc (Ubuntu 11.4.0-1ubuntu1~22.04.3) 11.4.0 |
| MPI | mpirun (Open MPI) 4.1.2 |
| entrada | data/entrada/longo.wav (1323000 amostras) |
| repeticoes | 5 (mediana) |

## Corretude

18/18 verificacoes passaram.

## Tempos

`t_local` e o processamento do bloco (maximo entre os processos);
`t_total` e a regiao paralela inteira, incluindo Scatterv/Gatherv.
O speedup e relativo a primeira linha da tabela.

| np | threads | t_local (s) | t_total (s) | speedup |
|---:|---:|---:|---:|---:|
| 1 | 1 | 0.125846 | 0.130288 | 1.00x |
| 1 | 2 | 0.100157 | 0.103923 | 1.25x |
| 1 | 4 | 0.057163 | 0.061129 | 2.13x |
| 1 | 8 | 0.041549 | 0.046403 | 2.81x |
| 2 | 1 | 0.072141 | 0.077405 | 1.68x |
| 2 | 2 | 0.063307 | 0.069666 | 1.87x |
| 2 | 4 | 0.044511 | 0.049795 | 2.62x |
| 2 | 8 | 0.025198 | 0.030469 | 4.28x |
| 4 | 1 | 0.028598 | 0.034050 | 3.83x |
| 4 | 2 | 0.036877 | 0.041082 | 3.17x |
| 4 | 4 | 0.021699 | 0.026525 | 4.91x |
| 4 | 8 | 0.028555 | 0.034011 | 3.83x * |
| 8 | 1 | 0.016532 | 0.019971 | 6.52x |
| 8 | 2 | 0.019842 | 0.024372 | 5.35x |
| 8 | 4 | 0.026170 | 0.032279 | 4.04x * |
| 8 | 8 | 0.120199 | 0.123889 | 1.05x * |

`*` = np x threads acima dos 28 nucleos disponiveis; ha disputa por CPU.

Referencia sequencial (np=1, `--no-parallel`): 0.109350 s.

Dados brutos (com minimo e maximo de cada configuracao): `resultados.csv`.
