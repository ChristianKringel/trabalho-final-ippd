#!/usr/bin/env bash
#
# Benchmark reprodutivel: bateria de corretude + varredura de desempenho.
# Um comando produz a tabela inteira e grava em docs/, com o ambiente medido.
#
# Uso:
#   scripts/benchmark.sh                          # bateria completa, padroes
#   scripts/benchmark.sh --reps 9                 # mais repeticoes
#   scripts/benchmark.sh --np "1 2 4" --threads "1 4"
#   scripts/benchmark.sh --audio data/entrada/longo.wav
#   scripts/benchmark.sh --hostfile maquinas.txt  # multi-no (Xivoco)
#   scripts/benchmark.sh --so-corretude
#
# Duas decisoes de medicao:
#   1. Tudo com '--bind-to none'. O padrao do OpenMPI amarra o processo a um
#      nucleo e prende ali todas as threads do rank: com '--bind-to core' o tempo
#      trava em ~0.14 s de 1 a 8 threads, sem amarracao cai de 0.20 s para 0.06 s.
#   2. Nada com --verbose: os logs saem de dentro da regiao cronometrada.
#
set -uo pipefail

# Sem isso o awk usa virgula decimal em maquina pt_BR e sai inconsistente com o CSV
export LC_ALL=C

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO"

BIN=./processa_audio
AUDIO=data/entrada/longo.wav
REPS=5
NP_LIST="1 2 4 8"
TH_LIST="1 2 4 8"
HOSTFILE=""
SO_CORRETUDE=0
OUT_MD=docs/resultados.md
OUT_CSV=docs/resultados.csv

while [[ $# -gt 0 ]]; do
    case "$1" in
        --audio)         AUDIO="$2"; shift 2 ;;
        --reps)          REPS="$2"; shift 2 ;;
        --np)            NP_LIST="$2"; shift 2 ;;
        --threads)       TH_LIST="$2"; shift 2 ;;
        --hostfile)      HOSTFILE="$2"; shift 2 ;;
        --so-corretude)  SO_CORRETUDE=1; shift ;;
        -h|--help)       sed -n '2,30p' "$0"; exit 0 ;;
        *) echo "opcao desconhecida: $1" >&2; exit 2 ;;
    esac
done

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

NUCLEOS="$(nproc)"
FALHAS=0
TOTAL=0

# ---------------------------------------------------------------- utilitarios

# Monta as opcoes do mpirun para um dado numero de processos.
opts_mpirun() {
    local np="$1"
    local -a o=(--bind-to none)
    if [[ -n "$HOSTFILE" ]]; then
        o+=(--hostfile "$HOSTFILE")
    elif (( np > NUCLEOS )); then
        o+=(--oversubscribe)
    fi
    printf '%s\n' "${o[@]}"
}

# Executa o programa e devolve a linha [BENCH]. Argumentos:
#   $1 np, $2 threads, $3 saida wav, resto: flags do programa
executar() {
    local np="$1" th="$2" saida="$3"; shift 3
    local -a mo=(); while IFS= read -r l; do mo+=("$l"); done < <(opts_mpirun "$np")
    OMP_NUM_THREADS="$th" mpirun -np "$np" "${mo[@]}" \
        "$BIN" "$AUDIO_ATUAL" "$saida" "$@" 2>"$TMP/err" | grep '^\[BENCH\]' || {
            echo "  !! execucao falhou (np=$np threads=$th $*)" >&2
            sed -n '1,4p' "$TMP/err" >&2
            return 1
        }
}

campo() { sed -n "s/.*$1=\([0-9.]*\).*/\1/p"; }

mediana() { printf '%s\n' "$@" | sort -g | awk '{v[NR]=$1} END{print v[int((NR+1)/2)]}'; }
minimo()  { printf '%s\n' "$@" | sort -g | head -1; }
maximo()  { printf '%s\n' "$@" | sort -g | tail -1; }

checar() {  # checar <descricao> <0|1 resultado>
    TOTAL=$((TOTAL + 1))
    if [[ "$2" == "0" ]]; then
        printf '  [ ok ] %s\n' "$1"
    else
        printf '  [FALHA] %s\n' "$1"
        FALHAS=$((FALHAS + 1))
    fi
}

# ------------------------------------------------------------------ preparo

echo "=== Preparo ==="
make --no-print-directory "$(basename "$BIN")" >/dev/null || { echo "falha ao compilar" >&2; exit 1; }
echo "  binario compilado"

if [[ ! -f "$AUDIO" ]]; then
    echo "  '$AUDIO' nao existe, gerando os presets"
    python3 scripts/gerar_wav_teste.py --preset >/dev/null
fi
[[ -f "$AUDIO" ]] || { echo "audio de entrada nao encontrado: $AUDIO" >&2; exit 1; }

# Audio curto e barato para a bateria de corretude
CURTO="$TMP/curto.wav"
python3 scripts/gerar_wav_teste.py --duracao 0.5 --freq 440 --saida "$CURTO" >/dev/null
# 44 amostras: com np=4 da blocos de exatamente 11, usado no teste de limite do halo
MINI="$TMP/mini.wav"
python3 scripts/gerar_wav_teste.py --duracao 0.001 --saida "$MINI" >/dev/null

# ---------------------------------------------------------------- corretude

echo
echo "=== Corretude ==="

AUDIO_ATUAL="$CURTO"

# 1. round-trip do wav_io, sem MPI
if make --no-print-directory test_wav >"$TMP/rt" 2>&1; then checar "round-trip do wav_io (le o que escreveu)" 0
else checar "round-trip do wav_io (le o que escreveu)" 1; sed -n '1,6p' "$TMP/rt"; fi

# 2. identidade: sem filtros, a saida tem que ser byte a byte igual a entrada.
#    Valida que Scatterv/Gatherv nao corrompem nem reordenam as amostras.
executar 1 4 "$TMP/id1.wav" >/dev/null
cmp -s "$CURTO" "$TMP/id1.wav"; checar "identidade: np=1 reproduz a entrada byte a byte" $?

# 3. a saida nao pode depender do numero de processos
for np in 2 4 8; do
    executar "$np" 4 "$TMP/id$np.wav" >/dev/null
    cmp -s "$TMP/id1.wav" "$TMP/id$np.wav"
    checar "identidade: np=1 == np=$np" $?
done

# 4. filtros de vizinhanca: so casam entre np diferentes se o halo estiver certo
for spec in "--convolucao 8" "--passa-alta 16" "--eco 100 0.4"; do
    executar 1 4 "$TMP/f1.wav" $spec >/dev/null
    for np in 4 8; do
        executar "$np" 4 "$TMP/f$np.wav" $spec >/dev/null
        cmp -s "$TMP/f1.wav" "$TMP/f$np.wav"
        checar "halo [$spec]: np=1 == np=$np" $?
    done
done

# 5. cadeia completa, incluindo a normalizacao (que depende do Allreduce global)
CADEIA="--convolucao 8 --eco 220 0.5 --normalizar 0.95"
executar 1 4 "$TMP/ch1.wav" $CADEIA >/dev/null
executar 8 4 "$TMP/ch8.wav" $CADEIA >/dev/null
cmp -s "$TMP/ch1.wav" "$TMP/ch8.wav"
checar "cadeia conv+eco+normalizar: np=1 == np=8" $?

# 6. a FFT tem 3 caminhos conforme as threads (1, 2-3, 4+) e todos devem concordar
executar 4 1 "$TMP/t.wav" "$TMP/th1.csv" >/dev/null
for th in 2 3 4 8; do
    executar 4 "$th" "$TMP/t.wav" "$TMP/th$th.csv" >/dev/null
    cmp -s "$TMP/th1.csv" "$TMP/th$th.csv"
    checar "metricas identicas: OMP=1 == OMP=$th (np=4)" $?
done

# 7. limite do halo: com 44 amostras e np=4 o menor bloco e 11, entao raio 11 deve
#    casar com np=1 e raio 12 deve ser recusado, nao errar em silencio
AUDIO_ATUAL="$MINI"
executar 1 2 "$TMP/lim1.wav" --convolucao 11 >/dev/null
executar 4 2 "$TMP/lim4.wav" --convolucao 11 >/dev/null
cmp -s "$TMP/lim1.wav" "$TMP/lim4.wav"
checar "halo no limite: raio == menor bloco (11) ainda casa com np=1" $?

if executar 4 2 "$TMP/lim_ruim.wav" --convolucao 12 >/dev/null 2>&1; then
    checar "halo invalido: raio > menor bloco e recusado" 1
else
    checar "halo invalido: raio > menor bloco e recusado" 0
fi

echo
echo "  $((TOTAL - FALHAS))/$TOTAL verificacoes passaram"
if (( FALHAS > 0 )); then
    echo "  ATENCAO: ha falhas de corretude; os tempos abaixo nao significam nada." >&2
fi

if (( SO_CORRETUDE == 1 )); then
    exit $(( FALHAS > 0 ? 1 : 0 ))
fi

# --------------------------------------------------------------- desempenho

AUDIO_ATUAL="$AUDIO"
AMOSTRAS="$(python3 -c "import wave,sys;print(wave.open(sys.argv[1]).getnframes())" "$AUDIO")"

echo
echo "=== Desempenho ==="
echo "  audio      : $AUDIO ($AMOSTRAS amostras)"
echo "  repeticoes : $REPS (reportada a mediana)"
echo "  np         : $NP_LIST"
echo "  threads    : $TH_LIST"
echo "  nucleos    : $NUCLEOS"
[[ -n "$HOSTFILE" ]] && echo "  hostfile   : $HOSTFILE"
echo

mkdir -p docs
: > "$OUT_CSV"
echo "np,threads,reps,t_local_mediana,t_local_min,t_local_max,t_total_mediana,t_total_min,t_total_max,oversubscrito" >> "$OUT_CSV"

# Referencia sequencial: 1 processo, 1 thread. Base de todos os speedups.
BASE=""
printf '  %-4s %-8s %-12s %-12s %s\n' "np" "threads" "t_local(s)" "t_total(s)" "speedup"

for np in $NP_LIST; do
    for th in $TH_LIST; do
        locais=(); totais=()
        for ((r = 0; r < REPS; r++)); do
            linha="$(executar "$np" "$th" "$TMP/bench.wav")" || continue
            locais+=("$(printf '%s' "$linha" | campo t_local_max)")
            totais+=("$(printf '%s' "$linha" | campo t_total)")
        done
        if (( ${#locais[@]} == 0 )); then
            printf '  %-4s %-8s %s\n' "$np" "$th" "sem medicao valida"
            continue
        fi

        lmed="$(mediana "${locais[@]}")"; lmin="$(minimo "${locais[@]}")"; lmax="$(maximo "${locais[@]}")"
        tmed="$(mediana "${totais[@]}")"; tmin="$(minimo "${totais[@]}")"; tmax="$(maximo "${totais[@]}")"

        over="nao"
        (( np * th > NUCLEOS )) && over="sim"

        [[ -z "$BASE" ]] && BASE="$tmed"
        sp="$(awk -v b="$BASE" -v t="$tmed" 'BEGIN{ if (t > 0) printf "%.2fx", b/t; else print "-" }')"
        marca=""; [[ "$over" == "sim" ]] && marca=" *"

        printf '  %-4s %-8s %-12s %-12s %s%s\n' "$np" "$th" "$lmed" "$tmed" "$sp" "$marca"
        echo "$np,$th,$REPS,$lmed,$lmin,$lmax,$tmed,$tmin,$tmax,$over" >> "$OUT_CSV"
    done
done

echo
echo "  * np x threads maior que $NUCLEOS nucleos: ha disputa por CPU, o numero nao"
echo "    e comparavel com as outras linhas."

# Referencia sequencial explicita, com o OpenMP desligado no proprio programa
seqs=()
for ((r = 0; r < REPS; r++)); do
    linha="$(executar 1 1 "$TMP/seq.wav" --no-parallel)" || continue
    seqs+=("$(printf '%s' "$linha" | campo t_total)")
done
SEQ="-"
if (( ${#seqs[@]} > 0 )); then
    SEQ="$(mediana "${seqs[@]}")"
    echo
    echo "  referencia sequencial (np=1, --no-parallel): $SEQ s"
fi

# ------------------------------------------------------------------ relatorio

{
    echo "# Resultados medidos"
    echo
    echo "Gerado por \`scripts/benchmark.sh\`. Para refazer:"
    echo
    echo '```'
    echo "scripts/benchmark.sh --audio $AUDIO --reps $REPS"
    echo '```'
    echo
    echo "## Ambiente"
    echo
    echo "| item | valor |"
    echo "|---|---|"
    echo "| data | $(date '+%Y-%m-%d %H:%M') |"
    echo "| host | $(hostname) |"
    echo "| nucleos | $NUCLEOS |"
    echo "| kernel | $(uname -sr) |"
    echo "| compilador | $(mpicc --version 2>/dev/null | head -1) |"
    echo "| MPI | $(mpirun --version 2>/dev/null | head -1) |"
    echo "| entrada | $AUDIO ($AMOSTRAS amostras) |"
    echo "| repeticoes | $REPS (mediana) |"
    [[ -n "$HOSTFILE" ]] && echo "| hostfile | $HOSTFILE |"
    echo
    echo "## Corretude"
    echo
    echo "$((TOTAL - FALHAS))/$TOTAL verificacoes passaram."
    echo
    echo "## Tempos"
    echo
    echo "\`t_local\` e o processamento do bloco (maximo entre os processos);"
    echo "\`t_total\` e a regiao paralela inteira, incluindo Scatterv/Gatherv."
    echo "O speedup e relativo a primeira linha da tabela."
    echo
    echo "| np | threads | t_local (s) | t_total (s) | speedup |"
    echo "|---:|---:|---:|---:|---:|"
    tail -n +2 "$OUT_CSV" | while IFS=, read -r np th _ lmed _ _ tmed _ _ over; do
        sp="$(awk -v b="$BASE" -v t="$tmed" 'BEGIN{ if (t > 0) printf "%.2fx", b/t; else print "-" }')"
        [[ "$over" == "sim" ]] && sp="$sp *"
        echo "| $np | $th | $lmed | $tmed | $sp |"
    done
    echo
    echo "\`*\` = np x threads acima dos $NUCLEOS nucleos disponiveis; ha disputa por CPU."
    echo
    echo "Referencia sequencial (np=1, \`--no-parallel\`): $SEQ s."
    echo
    echo "Dados brutos (com minimo e maximo de cada configuracao): \`resultados.csv\`."
} > "$OUT_MD"

echo
echo "=== Saida ==="
echo "  $OUT_MD"
echo "  $OUT_CSV"

exit $(( FALHAS > 0 ? 1 : 0 ))
