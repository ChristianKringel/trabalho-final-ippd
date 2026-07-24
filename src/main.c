#include <mpi.h>
#include <omp.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include "wav_io.h"
#include "mpi_utils.h"
#include "filtros.h"
#include "metrica.h"

// Orquestra o processamento paralelo do audio:
//   1. so o rank 0 le o .wav do disco (os workers podem nao compartilhar FS);
//   2. Bcast do formato + Scatterv das amostras em blocos contiguos;
//   3. cada processo filtra o seu bloco (OpenMP) e calcula as metricas dele;
//   4. Gatherv dos blocos + Gather das metricas, e o rank 0 escreve .wav e .csv.
// Os tempos (MPI_Wtime) e os logs com rank/thread evidenciam a divisao real do
// trabalho entre MPI e OpenMP.
//
// Uso:
//   mpirun -np <N> ./processa_audio <entrada.wav> <saida.wav> [metrica.csv]
//                  [--ganho G] [--threshold L] [--convolucao R] [--passa-alta R]
//                  [--eco ATRASO ATENUACAO] [--normalizar [ALVO]] [--no-parallel]
//                  [--verbose]
//
// Sem flags a saida sai igual a entrada, o que valida a malha de comunicacao.
//
// Dois tempos sao reportados: t_local e o processamento do bloco (reduzido com
// MAX e MIN entre os processos, para mostrar desbalanceio) e t_total e a regiao
// paralela inteira, do Bcast ao Gather, incluindo Scatterv/Gatherv. Sem
// --verbose nenhum printf sai de dentro dessas janelas, senao o I/O de terminal
// encaminhado pelo mpirun custaria da ordem do que se quer medir.

// Menor bloco entre todos os processos: usado para validar o raio dos filtros
// de vizinhanca, que trocam 'raio' amostras com os vizinhos
static int menor_bloco(const int *counts, int n_procs) {
    int menor = counts[0];
    for (int i = 1; i < n_procs; i++) {
        if (counts[i] < menor) menor = counts[i];
    }
    return menor;
}

// Filtros que usam a troca de halo
#define FILTRO_PASSA_BAIXA 0   // media movel (convolucao)
#define FILTRO_PASSA_ALTA  1   // original menos a media movel
#define FILTRO_ECO         2   // soma uma copia atrasada e atenuada

// Troca o halo com os vizinhos, aplica o filtro escolhido sobre o vetor
// estendido e copia o resultado de volta para o bloco. 'atenuacao_eco' so vale
// para tipo == FILTRO_ECO.
static void filtrar_com_vizinhanca(float *bloco, int n, int raio, int rank,
                                   int n_procs, const int *counts,
                                   int tipo, float atenuacao_eco) {
    if (raio > menor_bloco(counts, n_procs)) {
        if (rank == 0) {
            fprintf(stderr, "[Rank 0] Erro: raio do filtro (%d) e maior que o "
                    "menor bloco. Use menos processos ou um raio menor.\n", raio);
        }
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    float *estendido = malloc((n + 2 * raio) * sizeof(float));
    float *filtrado  = malloc((n > 0 ? n : 1) * sizeof(float));
    if (!estendido || !filtrado) {
        fprintf(stderr, "[Rank %d] Sem memoria para o filtro de vizinhanca.\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    trocar_halo(bloco, n, raio, rank, n_procs, estendido);
    if (tipo == FILTRO_PASSA_ALTA) {
        aplicar_passa_alta_media(estendido, filtrado, n, raio, rank);
    } else if (tipo == FILTRO_ECO) {
        aplicar_eco(estendido, filtrado, n, raio, atenuacao_eco, rank);
    } else {
        aplicar_convolucao_media(estendido, filtrado, n, raio, rank);
    }
    memcpy(bloco, filtrado, n * sizeof(float));

    free(estendido);
    free(filtrado);
}

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);

    int rank, n_procs;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &n_procs);

    if (argc < 3) {
        if (rank == 0) {
            fprintf(stderr,
                "Uso: mpirun -np <N> %s <entrada.wav> <saida.wav> [metrica.csv]"
                " [--ganho G] [--threshold L] [--convolucao R] [--passa-alta R]"
                " [--eco ATRASO ATENUACAO] [--normalizar [ALVO]] [--no-parallel]"
                " [--verbose]\n",
                argv[0]);
        }
        MPI_Finalize();
        return 1;
    }

    const char *caminho_entrada = argv[1];
    const char *caminho_saida   = argv[2];
    const char *caminho_metrica = NULL;

    float ganho = 1.0f;      // 1.0 = nao altera o volume
    float threshold = 0.0f;  // 0.0 = nao remove nada
    int raio_conv = 0;       // > 0 ativa o passa-baixa (media movel)
    int raio_pa = 0;         // > 0 ativa o passa-alta
    int atraso_eco = 0;      // > 0 ativa o eco (atraso em amostras)
    float aten_eco = 0.5f;   // volume do eco (0 a 1)
    int normalizar = 0;      // se 1, normaliza o pico
    float alvo_norm = 0.99f; // pico alvo da normalizacao (0 a 1)
    int no_parallel = 0;     // se 1, forca OpenMP a usar 1 thread
    int verbose = 0;         // se 1, liga os logs por rank/thread (contamina o tempo)

    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "--ganho") == 0 && i + 1 < argc) {
            ganho = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--threshold") == 0 && i + 1 < argc) {
            threshold = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--convolucao") == 0 && i + 1 < argc) {
            raio_conv = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--passa-alta") == 0 && i + 1 < argc) {
            raio_pa = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--eco") == 0 && i + 2 < argc) {
            atraso_eco = atoi(argv[++i]);
            aten_eco   = (float)atof(argv[++i]);
        } else if (strcmp(argv[i], "--normalizar") == 0) {
            // alvo opcional: so consome o proximo arg se parecer numero, para
            // nao confundir com o arquivo de metricas ou com outra flag
            normalizar = 1;
            if (i + 1 < argc &&
                (isdigit((unsigned char)argv[i + 1][0]) || argv[i + 1][0] == '.')) {
                alvo_norm = (float)atof(argv[++i]);
            }
        } else if (strcmp(argv[i], "--no-parallel") == 0) {
            no_parallel = 1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (argv[i][0] != '-') {
            // primeiro posicional extra e o arquivo de metricas
            if (caminho_metrica == NULL) {
                caminho_metrica = argv[i];
            }
        }
    }

    if (no_parallel) {
        omp_set_num_threads(1);
    }
    filtros_set_verbose(verbose);

    // Sem OMP_NUM_THREADS, o OpenMP cria uma thread por nucleo em cada processo:
    // 'mpirun -np 4' em 28 nucleos daria 112 threads, e as barreiras da FFT
    // tropecam no escalonador. Repartimos os nucleos entre os processos deste no
    // (nao do mundo, senao multi-no ficaria subutilizado).
    int procs_neste_no = n_procs;
    {
        MPI_Comm comm_no;
        if (MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, rank,
                                MPI_INFO_NULL, &comm_no) == MPI_SUCCESS) {
            MPI_Comm_size(comm_no, &procs_neste_no);
            MPI_Comm_free(&comm_no);
        }
    }
    // omp_get_num_procs() devolve so os nucleos visiveis pela mascara de
    // afinidade. Se o mpirun ja amarrou o rank (o padrao dele), a mascara ja
    // fez o rateio e dividir de novo daria uma thread so.
    long cpus_maquina  = sysconf(_SC_NPROCESSORS_ONLN);
    int  cpus_visiveis = omp_get_num_procs();
    if (!no_parallel && getenv("OMP_NUM_THREADS") == NULL) {
        int por_processo = (cpus_maquina > 0 && cpus_visiveis >= (int)cpus_maquina)
            ? cpus_visiveis / procs_neste_no   // sem amarracao: reparte
            : cpus_visiveis;                   // amarrado: a mascara ja repartiu
        if (por_processo < 1) {
            por_processo = 1;
        }
        omp_set_num_threads(por_processo);
    }

    WavInfo info;
    float *audio_total = NULL;   // so o rank 0 aloca o audio inteiro

    // Passo 1: so o rank 0 le a entrada
    if (rank == 0) {
        printf("[Rank 0] Lendo arquivo de entrada '%s' do disco.\n", caminho_entrada);
        if (ler_wav(caminho_entrada, &info, &audio_total) != 0) {
            fprintf(stderr, "[Rank 0] Erro ao ler a entrada. Abortando.\n");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
        printf("[Rank 0] Audio: %u amostras, %u Hz, %u canal(is), %u bits.\n",
               info.num_amostras, info.sample_rate, info.num_canais, info.bits_amostra);
        printf("[Rank 0] Filtros: ganho=%.3f threshold=%.4f convolucao(raio)=%d passa-alta(raio)=%d eco(atraso=%d,aten=%.2f) normalizar=%s(alvo=%.2f) | processos=%d | %s\n",
               ganho, threshold, raio_conv, raio_pa, atraso_eco, aten_eco,
               normalizar ? "sim" : "nao", alvo_norm, n_procs,
               no_parallel ? "OpenMP com 1 thread" : "OpenMP com varias threads");
        printf("[Rank 0] Threads OpenMP por processo: %d (%d processo(s) neste no, "
               "%d nucleos)%s\n",
               omp_get_max_threads(), procs_neste_no, omp_get_num_procs(),
               getenv("OMP_NUM_THREADS") ? " [de OMP_NUM_THREADS]" : " [automatico]");
        if (info.num_canais != 1) {
            printf("[Rank 0] Aviso: audio nao e mono; tratando os valores de "
                   "forma intercalada (recomendado usar mono).\n");
        }
        if (verbose) {
            printf("[Rank 0] Aviso: --verbose imprime de dentro da regiao "
                   "cronometrada; os tempos abaixo saem inflados. Meca sem "
                   "--verbose.\n");
        }
        // Escolha explicita nao e sobreposta, mas fica avisada
        int total_threads = procs_neste_no * omp_get_max_threads();
        if (!no_parallel && total_threads > omp_get_num_procs()) {
            printf("[Rank 0] Aviso: %d processo(s) x %d thread(s) = %d threads "
                   "para %d nucleo(s) neste no. Havera disputa por CPU e os "
                   "tempos nao serao representativos.\n",
                   procs_neste_no, omp_get_max_threads(), total_threads,
                   omp_get_num_procs());
        }
    }

    // Marca o inicio da regiao paralela para medir o tempo de parede
    MPI_Barrier(MPI_COMM_WORLD);
    double t_inicio = MPI_Wtime();

    // Passo 2: todos ficam sabendo o formato/tamanho do audio
    MPI_Bcast(&info, sizeof(WavInfo), MPI_BYTE, 0, MPI_COMM_WORLD);

    int total_valores = (int)(info.num_amostras * info.num_canais);

    // Cada processo calcula a mesma particao de forma deterministica
    int *counts = malloc(n_procs * sizeof(int));
    int *displs = malloc(n_procs * sizeof(int));
    if (!counts || !displs) {
        fprintf(stderr, "[Rank %d] Sem memoria para a particao.\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    calcular_particao(total_valores, n_procs, counts, displs);

    int meu_tamanho = counts[rank];
    int meu_offset  = displs[rank];

    float *bloco_local = malloc((meu_tamanho > 0 ? meu_tamanho : 1) * sizeof(float));
    if (!bloco_local) {
        fprintf(stderr, "[Rank %d] Sem memoria para o bloco local.\n", rank);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }

    // Passo 3: distribuicao dos blocos
    MPI_Scatterv(audio_total, counts, displs, MPI_FLOAT,
                 bloco_local, meu_tamanho, MPI_FLOAT,
                 0, MPI_COMM_WORLD);

    int n_threads = omp_get_max_threads();
    if (verbose) {
        printf("[Rank %d] Recebi bloco de %d amostras (offset %d), vou usar %d thread(s) OpenMP.\n",
               rank, meu_tamanho, meu_offset, n_threads);
    }

    // Passo 4: filtros sobre o bloco local
    double t_local_ini = MPI_Wtime();

    if (ganho != 1.0f) {
        aplicar_filtro_ganho(bloco_local, meu_tamanho, ganho, rank);
    }
    if (threshold > 0.0f) {
        aplicar_threshold_ruido(bloco_local, meu_tamanho, threshold, rank);
    }

    // Filtros de vizinhanca: passa-baixa e passa-alta usam a mesma media movel,
    // e se os dois forem pedidos o passa-baixa vem primeiro
    if (raio_conv > 0) {
        filtrar_com_vizinhanca(bloco_local, meu_tamanho, raio_conv, rank,
                               n_procs, counts, FILTRO_PASSA_BAIXA, 0.0f);
    }
    if (raio_pa > 0) {
        filtrar_com_vizinhanca(bloco_local, meu_tamanho, raio_pa, rank,
                               n_procs, counts, FILTRO_PASSA_ALTA, 0.0f);
    }

    // Eco: halo com raio = atraso, logo o atraso tambem nao pode passar do
    // menor bloco
    if (atraso_eco > 0) {
        filtrar_com_vizinhanca(bloco_local, meu_tamanho, atraso_eco, rank,
                               n_procs, counts, FILTRO_ECO, aten_eco);
    }

    // Normalizacao: precisa do pico GLOBAL, obtido com Allreduce(MAX). O fator
    // e igual em todos os processos, entao a saida nao depende de quantos sao.
    // Fica por ultimo para considerar o efeito dos filtros anteriores (o eco,
    // por exemplo, pode aumentar o pico).
    if (normalizar) {
        float pico_local = pico_local_bloco(bloco_local, meu_tamanho);
        float pico_global = 0.0f;
        MPI_Allreduce(&pico_local, &pico_global, 1, MPI_FLOAT, MPI_MAX,
                      MPI_COMM_WORLD);

        float fator = (pico_global > 1e-9f) ? (alvo_norm / pico_global) : 1.0f;
        if (rank == 0 && verbose) {
            printf("[Rank 0] Normalizacao: pico global=%.4f alvo=%.4f fator=%.4f\n",
                   pico_global, alvo_norm, fator);
        }
        if (pico_global > 1e-9f) {
            aplicar_normalizacao(bloco_local, meu_tamanho, fator, rank);
        }
    }

    MetricaBloco minha_metrica =
        calcular_metrica(bloco_local, meu_tamanho, rank, meu_offset, info.sample_rate);

    double t_local = MPI_Wtime() - t_local_ini;

    if (verbose) {
        printf("[Rank %d] Bloco processado em %.4f s | RMS=%.4f pico=%.4f freq_dom=%.1f Hz\n",
               rank, t_local,
               minha_metrica.rms, minha_metrica.pico, minha_metrica.freq_dominante);
    }

    // Passo 5: reune os blocos processados e as metricas no rank 0
    MPI_Gatherv(bloco_local, meu_tamanho, MPI_FLOAT,
                audio_total, counts, displs, MPI_FLOAT,
                0, MPI_COMM_WORLD);

    MetricaBloco *todas_metricas = NULL;
    if (rank == 0) {
        todas_metricas = malloc(n_procs * sizeof(MetricaBloco));
        if (!todas_metricas) {
            fprintf(stderr, "[Rank 0] Sem memoria para reunir as metricas.\n");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }
    }
    MPI_Gather(&minha_metrica, sizeof(MetricaBloco), MPI_BYTE,
               todas_metricas, sizeof(MetricaBloco), MPI_BYTE,
               0, MPI_COMM_WORLD);

    MPI_Barrier(MPI_COMM_WORLD);
    double t_fim = MPI_Wtime();
    double t_total = t_fim - t_inicio;

    // Depois de t_fim: e instrumentacao, nao entra na conta do tempo total
    double t_local_max = 0.0, t_local_min = 0.0;
    MPI_Reduce(&t_local, &t_local_max, 1, MPI_DOUBLE, MPI_MAX, 0, MPI_COMM_WORLD);
    MPI_Reduce(&t_local, &t_local_min, 1, MPI_DOUBLE, MPI_MIN, 0, MPI_COMM_WORLD);

    // Passo 6: so o rank 0 escreve os resultados
    if (rank == 0) {
        printf("[Rank 0] Escrevendo arquivo de saida '%s' no disco.\n", caminho_saida);
        if (escrever_wav(caminho_saida, &info, audio_total) != 0) {
            fprintf(stderr, "[Rank 0] Erro ao escrever a saida.\n");
            MPI_Abort(MPI_COMM_WORLD, 1);
        }

        if (caminho_metrica != NULL) {
            printf("[Rank 0] Escrevendo metricas em '%s'.\n", caminho_metrica);
            if (escrever_metricas_csv(caminho_metrica, todas_metricas, n_procs) != 0) {
                fprintf(stderr, "[Rank 0] Erro ao escrever as metricas.\n");
                MPI_Abort(MPI_COMM_WORLD, 1);
            }
        }

        double desbalanceio = (t_local_max > 0.0)
            ? 100.0 * (t_local_max - t_local_min) / t_local_max : 0.0;

        printf("[Rank 0] Processamento do bloco: max=%.4f s min=%.4f s "
               "(desbalanceio %.1f%%)\n",
               t_local_max, t_local_min, desbalanceio);
        printf("[Rank 0] Tempo total (parede) com %d processo(s): %.4f s\n",
               n_procs, t_total);

        // Formato fixo para o benchmark.sh ler sem depender do texto acima
        printf("[BENCH] np=%d threads=%d t_local_max=%.6f t_local_min=%.6f "
               "t_total=%.6f\n",
               n_procs, n_threads, t_local_max, t_local_min, t_total);
    }

    free(bloco_local);
    free(counts);
    free(displs);
    if (rank == 0) {
        free(audio_total);
        free(todas_metricas);
    }

    MPI_Finalize();
    return 0;
}
