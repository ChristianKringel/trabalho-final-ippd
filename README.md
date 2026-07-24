# Processamento de sinais de áudio em lote com MPI e OpenMP

Este trabalho foi feito para a disciplina de Programação Paralela e tem como
tema o processamento de sinais de áudio distribuído entre vários processos. A
ideia central é pegar um arquivo de áudio, dividir as amostras entre os
processos, aplicar alguns filtros e calcular métricas sobre o sinal, tudo em
paralelo, e no final juntar o resultado de volta em um único arquivo. Usamos
MPI para distribuir os pedaços do áudio entre os processos e OpenMP para
paralelizar o trabalho dentro de cada pedaço.

## Sumário

- [O problema e por que ele exige um cuidado especial](#o-problema-e-por-que-ele-exige-um-cuidado-especial)
- [Como o trabalho é dividido](#como-o-trabalho-é-dividido)
- [As métricas e a transformada de Fourier](#as-métricas-e-a-transformada-de-fourier)
- [Como os logs contam a história da execução](#como-os-logs-contam-a-história-da-execução)
- [Como compilar](#como-compilar)
- [Onde o trabalho fica no Xivoco](#onde-o-trabalho-fica-no-xivoco)
- [Como executar](#como-executar)
- [O exemplo completo: limpando um áudio de voz real](#o-exemplo-completo-limpando-um-áudio-de-voz-real)
- [Os filtros disponíveis](#os-filtros-disponíveis)
- [Áudios de entrada e de saída já prontos](#áudios-de-entrada-e-de-saída-já-prontos)
- [Convertendo formatos com ffmpeg](#convertendo-formatos-com-ffmpeg)
- [Como medir: `scripts/benchmark.sh`](#como-medir-scriptsbenchmarksh)
- [Sobre os testes que fizemos](#sobre-os-testes-que-fizemos)
- [Integrantes do grupo](#integrantes-do-grupo)

## O problema e por que ele exige um cuidado especial

À primeira vista, processar um áudio em paralelo parece simples: basta dividir
o vetor de amostras em partes iguais e mandar cada parte para um processo. O
complicador aqui é que a máquina virtual onde o trabalho roda não compartilha
disco entre os processos. Ou seja, se cada processo tentasse abrir o arquivo de
áudio por conta própria, a maioria deles simplesmente não encontraria o arquivo,
porque ele existe apenas na máquina de um dos processos.

Por causa disso, decidimos que apenas o processo de rank 0 tem contato com o
disco. Ele é o único que lê o arquivo de entrada e o único que escreve os
arquivos de saída. Todos os outros processos nunca abrem arquivo nenhum; eles
recebem as amostras que precisam processar diretamente do rank 0, através de
mensagens MPI, e devolvem o resultado da mesma forma. Essa decisão aparece o
tempo todo no código e é, na prática, o que torna a solução compatível com o
ambiente de execução.

## Como o trabalho é dividido

O rank 0 lê o arquivo inteiro e converte as amostras para números em ponto
flutuante no intervalo de menos um a mais um, que é um formato mais confortável
para aplicar filtros e para a transformada de Fourier. Em seguida ele calcula
como dividir o total de amostras entre os processos. Como o número de amostras
quase nunca é um múltiplo exato do número de processos, não dá para simplesmente
dividir em partes iguais. Por isso usamos as versões Scatterv e Gatherv das
operações de distribuição, que permitem que cada bloco tenha um tamanho
diferente. O resto da divisão é distribuído de um em um entre os primeiros
processos, de modo que a diferença de tamanho entre qualquer par de blocos seja
no máximo de uma amostra.

Depois de espalhar os blocos com Scatterv, cada processo trabalha apenas na sua
parte, sem precisar conhecer o restante do áudio. Isso é possível porque os
filtros que implementamos primeiro são filtros ponto a ponto, em que o valor de
saída de cada amostra depende apenas da própria amostra de entrada, e não dos
vizinhos. Um filtro de ganho, que só multiplica cada amostra por um fator, e um
filtro de remoção de ruído por limiar, que zera as amostras de baixa amplitude,
são os dois exemplos que usamos. Como não há dependência entre amostras
vizinhas, não é preciso trocar nenhuma informação de borda entre os blocos, e a
divisão fica bem limpa.

Dentro de cada bloco, o laço que percorre as amostras é paralelizado com OpenMP,
de forma que várias threads dividem o trabalho do bloco entre si. Assim temos
dois níveis de paralelismo acontecendo ao mesmo tempo: o MPI separando o áudio
em blocos entre processos, e o OpenMP dividindo cada bloco entre threads.

Como um passo a mais, implementamos também um filtro por convolução, uma média
móvel que funciona como um passa-baixa. Esse filtro é diferente dos anteriores
porque cada amostra de saída é a média de um trecho ao redor dela, ou seja, ela
depende dos vizinhos. Nas bordas de cada bloco, alguns desses vizinhos estão no
bloco do processo ao lado. Para resolver isso, antes de aplicar a convolução
cada processo troca com os vizinhos uma pequena margem de amostras, que
costumamos chamar de halo, usando uma comunicação em que se envia e se recebe ao
mesmo tempo. Com essa margem no lugar, o filtro roda normalmente e o resultado
nas fronteiras entre blocos fica igual ao que sairia se o áudio tivesse sido
processado inteiro em um único pedaço. Nas duas pontas do áudio, onde não há
vizinho, a margem é preenchida com zeros.

Terminado o processamento, o rank 0 recolhe todos os blocos de volta com Gatherv,
remontando o áudio na ordem correta, e escreve o arquivo de saída. As métricas
de cada bloco são recolhidas separadamente e gravadas em um arquivo de texto no
formato de tabela.

## As métricas e a transformada de Fourier

Além de filtrar o áudio, cada processo calcula três métricas sobre o seu bloco:
a energia RMS, que dá uma ideia do volume médio do trecho, o pico de amplitude,
que ajuda a perceber se o sinal está saturando, e a frequência dominante, que é
a frequência que mais aparece naquele pedaço do áudio. Essa última só é possível
de calcular por causa da transformada de Fourier.

Optamos por implementar a transformada rápida de Fourier nós mesmos, pelo
algoritmo de Cooley-Tukey na forma iterativa, em vez de usar uma biblioteca
pronta. A escolha foi tanto para não depender de nenhuma biblioteca externa que
pudesse não estar instalada no ambiente quanto porque escrever a transformada à
mão deixa mais claro o que está acontecendo e combina melhor com o objetivo da
disciplina. Antes de calcular o espectro, aplicamos uma janela de Hann sobre o
trecho, o que reduz o vazamento espectral e deixa a leitura da frequência
dominante mais precisa.

## Como os logs contam a história da execução

Como a avaliação acontece observando a execução ao vivo no terminal, tratamos os
logs como parte do resultado, e não como algo descartável de depuração. Com a
flag `--verbose`, cada processo imprime, com o seu número de rank na frente, o
tamanho do bloco que recebeu e a posição em que ele começa no áudio original, e
cada thread imprime a faixa de amostras que ficou sob sua responsabilidade, com o
rank e o número da thread no início da linha, o que torna visível que o OpenMP
está de fato dividindo o trabalho.

Esses logs, porém, são impressos de dentro da região cronometrada, e um `printf`
por thread custa tempo suficiente para inflar a medição. Por isso eles são
opcionais: **sem `--verbose` o programa não imprime nada de dentro das janelas de
tempo**, e o próprio programa avisa disso quando a flag é usada. A regra prática
é usar `--verbose` para demonstrar a divisão do trabalho e rodar sem ele para
medir desempenho.

Em qualquer um dos modos, no fim o rank 0 imprime o desbalanceio entre os blocos,
o tempo total de parede medido com o relógio do próprio MPI, e uma linha
`[BENCH]` com os números crus, fácil de recortar em scripts de medição:

```
[Rank 0] Processamento do bloco: max=0.4435 s min=0.4159 s (desbalanceio 6.2%)
[Rank 0] Tempo total (parede) com 4 processo(s): 0.4449 s
[BENCH] np=4 threads=28 t_local_max=0.443528 t_local_min=0.415858 t_total=0.444911
```

O número de linhas de thread cresce com o produto de processos por threads e por
filtros ativos, então em uma varredura grande vale filtrá-las para ver só o
essencial:

```
mpirun -np 4 ./processa_audio <entrada> <saida> --verbose 2>&1 | grep -vE "Thread [0-9]+\]"
```

## Como compilar

O projeto é escrito em C e depende apenas do compilador MPI com suporte a OpenMP
e da biblioteca matemática padrão. Não há nenhuma biblioteca externa para
instalar: a leitura e a escrita de WAV, a FFT e os filtros são todos escritos à
mão. Para compilar, basta usar o Makefile na raiz:

```
make
```

Isso gera o executável `processa_audio` na raiz do projeto. Os outros alvos do
Makefile são:

| alvo | o que faz |
| ---- | --------- |
| `make` | compila o `processa_audio` |
| `make clean` | apaga os objetos e os binários |
| `make test` | gera um áudio de um segundo e roda uma execução de sanidade com dois processos |
| `make test_wav` | round-trip do leitor e escritor de WAV, só com `gcc`, sem MPI |
| `make check` | bateria de corretude completa, dezoito verificações |
| `make bench` | corretude mais varredura de desempenho, grava em `docs/` |

O ambiente em que desenvolvemos usa Open MPI 4.1.2 e `mpicc` com `-fopenmp`, em
uma máquina de 28 núcleos.

## Onde o trabalho fica no Xivoco

> **A preencher.** Esta seção descreve onde o projeto está hospedado no Xivoco e
> como chegar até ele para rodar a demonstração na apresentação.

- Endereço e forma de acesso: `TODO`
- Usuário: `TODO`
- Caminho do projeto na máquina: `TODO`
- Como carregar o ambiente MPI, se for necessário: `TODO`
- Número de núcleos por nó e limite recomendado de processos: `TODO`

Para rodar distribuído em mais de um nó, monte um arquivo de hosts e passe para o
`mpirun` com `--hostfile`. O `benchmark.sh` também aceita a opção e repassa para
todas as execuções, então a bateria inteira roda multi-nó com um comando:

```
scripts/benchmark.sh --hostfile maquinas.txt
```

Vale reforçar aqui a decisão de projeto descrita no começo: só o rank 0 toca o
disco. Isso é justamente o que permite rodar em vários nós sem sistema de arquivos
compartilhado — os outros processos recebem as amostras por mensagem e não precisam
que o arquivo de entrada exista na máquina deles. Na prática, o arquivo de áudio só
precisa estar no nó onde o rank 0 for escalonado.

Uma consequência da repartição automática de threads também aparece aqui: o
programa conta quantos processos existem **no mesmo nó**, não no mundo, para
decidir quantas threads abrir. Com dois nós de 28 núcleos e oito processos, são
quatro processos por nó e sete threads cada, e não oito processos disputando um
mesmo total.

## Como executar

A forma geral de rodar é passar o arquivo de entrada, o arquivo de saída e,
opcionalmente, o arquivo de métricas, seguidos das flags dos filtros:

```
mpirun -np <N> ./processa_audio <entrada.wav> <saida.wav> [metrica.csv] [flags]
```

O primeiro e o segundo argumentos são obrigatórios. O terceiro é opcional: se
for informado um caminho que não comece com `-`, ele é usado como arquivo de
métricas em CSV; se for omitido, as métricas só aparecem no terminal.

Sem nenhuma flag o programa não altera o áudio, apenas o distribui, processa e
junta de volta, o que é útil para verificar que a saída sai idêntica à entrada:

```
mpirun -np 4 ./processa_audio data/entrada/medio.wav data/saida/saida.wav data/saida/metrica.csv
```

Sobre a entrada, é importante saber que o programa lê **apenas WAV PCM de 16
bits** (`src/wav_io.c` recusa qualquer outro formato com uma mensagem clara). O
número de canais e a taxa de amostragem são livres e vêm do cabeçalho do
arquivo, então tanto 44100 Hz quanto 48000 Hz funcionam. Áudio em MP3, OGG ou
WAV de 24 ou 32 bits precisa ser convertido antes; veja a seção
[Convertendo formatos com ffmpeg](#convertendo-formatos-com-ffmpeg).

Quanto ao número de processos, vale lembrar que os filtros de vizinhança trocam
halo com os vizinhos e por isso exigem que o raio (ou o atraso, no caso do eco)
seja menor ou igual ao menor bloco. Com áudios curtos e muitos processos os
blocos ficam pequenos e o programa aborta avisando disso; a saída é usar menos
processos ou um raio menor.

Sobre o número de threads, não é preciso configurar nada: **o programa reparte os
núcleos entre os processos do mesmo nó automaticamente**. Se `OMP_NUM_THREADS`
não estiver definido, cada processo abre `núcleos / processos no nó` threads, e o
rank 0 informa a conta no início da execução:

```
[Rank 0] Threads OpenMP por processo: 7 (4 processo(s) neste no, 28 nucleos) [automatico]
```

Isso existe porque o padrão do OpenMP é abrir uma thread por núcleo em cada
processo, o que com quatro processos em 28 núcleos daria 112 threads disputando
28 núcleos. Definir `OMP_NUM_THREADS` à mão continua funcionando e tem
precedência; nesse caso a mesma linha aparece marcada com `[de OMP_NUM_THREADS]`.
A repartição usa só os processos do próprio nó, e não do mundo, para que uma
execução multi-nó não fique subutilizada.

Vale também um aviso sobre o `mpirun`: por padrão o OpenMPI amarra cada processo a
um núcleo, o que prende todas as threads daquele rank no mesmo núcleo e faz o
OpenMP parecer inútil. Para qualquer execução em que o tempo importe, use
`--bind-to none`:

```
mpirun -np 4 --bind-to none ./processa_audio ...
```

## O exemplo completo: limpando um áudio de voz real

Esta é a demonstração que consideramos a mais interessante do trabalho, porque
ela sai de um áudio de verdade, com ruído de verdade, e chega em um resultado que
dá para ouvir a diferença. O arquivo é uma mensagem de voz de nove segundos
(`data/entrada/testeLucas.ogg`), e o caminho completo tem três etapas.

**Primeiro, converter para o formato que o programa lê.** A mensagem veio em OGG
com codec Opus, que o nosso leitor não aceita:

```
ffmpeg -i data/entrada/testeLucas.ogg -c:a pcm_s16le data/entrada/testeLucas.wav
```

O resultado é um WAV PCM de 16 bits, mono, 48000 Hz, com 434568 amostras.

**Segundo, descobrir onde está o ruído para escolher o filtro certo.** Medindo o
espectro das janelas mais silenciosas do arquivo, o piso de ruído tem RMS de
0,0078 (−42,2 dBFS) e **78% da sua energia está abaixo de 300 Hz**, enquanto a
voz concentra 59% da energia entre 300 Hz e 1 kHz. O ruído deste áudio é ronco
de baixa frequência, não chiado, o que aponta diretamente para o filtro
passa-alta. Uma pista disso aparece de graça no próprio programa: rodando sem
filtro nenhum, um dos blocos acusa frequência dominante de 60,1 Hz, que é a cara
de um zumbido de rede elétrica.

**Terceiro, aplicar o passa-alta e normalizar:**

```
mpirun -np 4 --bind-to none ./processa_audio \
  data/entrada/testeLucas.wav \
  data/saida/lucas_pa48.wav \
  data/saida/lucas_pa48.csv \
  --passa-alta 48 --normalizar 0.95
```

A saída enxuta, que é a do modo de medição, fica assim:

```
[Rank 0] Lendo arquivo de entrada 'data/entrada/testeLucas.wav' do disco.
[Rank 0] Audio: 434568 amostras, 48000 Hz, 1 canal(is), 16 bits.
[Rank 0] Filtros: ganho=1.000 threshold=0.0000 convolucao(raio)=0 passa-alta(raio)=48 eco(atraso=0,aten=0.50) normalizar=sim(alvo=0.95) | processos=4 | OpenMP com varias threads
[Rank 0] Threads OpenMP por processo: 7 (4 processo(s) neste no, 28 nucleos) [automatico]
[Rank 0] Escrevendo arquivo de saida 'data/saida/lucas_pa48.wav' no disco.
[Rank 0] Escrevendo metricas em 'data/saida/lucas_pa48.csv'.
[Rank 0] Processamento do bloco: max=0.0214 s min=0.0160 s (desbalanceio 25.3%)
[Rank 0] Tempo total (parede) com 4 processo(s): 0.0231 s
[BENCH] np=4 threads=7 t_local_max=0.021398 t_local_min=0.015990 t_total=0.023135
```

Para a demonstração, acrescentando `--verbose` e filtrando as linhas de thread,
aparece a divisão do trabalho:

```
[Rank 0] Lendo arquivo de entrada 'data/entrada/testeLucas.wav' do disco.
[Rank 0] Audio: 434568 amostras, 48000 Hz, 1 canal(is), 16 bits.
[Rank 0] Filtros: ganho=1.000 threshold=0.0000 convolucao(raio)=0 passa-alta(raio)=48 eco(atraso=0,aten=0.50) normalizar=sim(alvo=0.95) | processos=4 | OpenMP com varias threads
[Rank 0] Threads OpenMP por processo: 7 (4 processo(s) neste no, 28 nucleos) [automatico]
[Rank 0] Aviso: --verbose imprime de dentro da regiao cronometrada; os tempos abaixo saem inflados. Meca sem --verbose.
[Rank 1] Recebi bloco de 108642 amostras (offset 108642), vou usar 7 thread(s) OpenMP.
[Rank 2] Recebi bloco de 108642 amostras (offset 217284), vou usar 7 thread(s) OpenMP.
[Rank 3] Recebi bloco de 108642 amostras (offset 325926), vou usar 7 thread(s) OpenMP.
[Rank 0] Recebi bloco de 108642 amostras (offset 0), vou usar 7 thread(s) OpenMP.
[Rank 0] Normalizacao: pico global=0.8105 alvo=0.9500 fator=1.1722
[Rank 2] Bloco processado em 0.0168 s | RMS=0.0673 pico=0.5944 freq_dom=388.2 Hz
[Rank 1] Bloco processado em 0.0174 s | RMS=0.0761 pico=0.6654 freq_dom=417.5 Hz
[Rank 3] Bloco processado em 0.0213 s | RMS=0.0456 pico=0.4371 freq_dom=492.2 Hz
[Rank 0] Bloco processado em 0.0239 s | RMS=0.0776 pico=0.9500 freq_dom=566.9 Hz
[Rank 0] Escrevendo arquivo de saida '/tmp/v.wav' no disco.
[Rank 0] Escrevendo metricas em '/tmp/v.csv'.
[Rank 0] Processamento do bloco: max=0.0239 s min=0.0168 s (desbalanceio 29.6%)
[Rank 0] Tempo total (parede) com 4 processo(s): 0.0260 s
```

Comparando os dois blocos dá para ver o custo dos logs: o mesmo trabalho sai em
0,0231 s sem `--verbose` e em 0,0260 s com ele, uma diferença de 13% que não tem
nada a ver com o processamento.

Esse log mostra bem o que o trabalho faz. As 434568 amostras foram divididas em
quatro blocos iguais de 108642, cada um com o seu offset; a normalização
encontrou o pico global de 0,8105 por redução coletiva e aplicou o mesmo fator
de 1,1722 em todos os processos; e a frequência dominante do bloco que antes
acusava 60,1 Hz passou a acusar 417,5 Hz, ou seja, o zumbido saiu e o que sobrou
foi a voz.

O arquivo de métricas gerado é uma tabela com uma linha por bloco:

```
rank,offset,num_amostras,rms,pico,freq_dominante_hz
0,0,108642,0.077571,0.950000,566.89
1,108642,108642,0.076087,0.665350,417.48
2,217284,108642,0.067267,0.594434,388.18
3,325926,108642,0.045575,0.437129,492.19
```

**O resultado medido.** Comparando as janelas mais silenciosas e as mais altas
do arquivo, sempre nas mesmas posições em todas as versões:

| versão                       | SNR     | piso de ruído | ruído abaixo de 300 Hz |
| ---------------------------- | ------- | ------------- | ---------------------- |
| original                     | 29,1 dB | −42,2 dBFS    | 78,4%                  |
| `--threshold 0.01`           | 30,9 dB | −41,3 dBFS    | 54,2%                  |
| `--passa-alta 32`            | 33,5 dB | −50,1 dBFS    | 3,5%                   |
| **`--passa-alta 48`**        | 35,0 dB | −48,7 dBFS    | 9,5%                   |
| `--passa-alta 64`            | 35,3 dB | −46,8 dBFS    | 17,7%                  |

O raio 48 é o melhor equilíbrio: atenua 32 dB em 60 Hz e 24 dB em 100 Hz, mas
custa só 6 dB em 300 Hz, então derruba o ronco sem afinar a voz. O raio 32 limpa
mais os graves, porém já tira 12 dB em 300 Hz e 4,4 dB em 500 Hz, onde está o
corpo da fala. Os três raios estão gravados em `data/saida/` para comparação.

**A tentativa que deu errado, e por quê.** A primeira coisa que tentamos neste
áudio foi `--threshold 0.01 --normalizar`, que é o filtro que o próprio nome
sugere para remover ruído, e o resultado ficou audivelmente **pior** que o
original. O motivo é instrutivo: o `--threshold` é um gate por amostra, que zera
qualquer amostra com módulo abaixo do limiar sem olhar o contexto. Como o piso de
ruído deste arquivo é 0,0078, praticamente em cima do limiar de 0,01, o gate não
cortou o ruído nas pausas: ele cortou dentro da própria forma de onda da fala,
zerando 30,4% de todas as amostras, incluindo todo cruzamento por zero no meio
da voz alta. Isso não remove ruído, adiciona distorção harmônica. E o
`--normalizar` sem alvo, em seguida, amplificou a distorção junto com o resto.
Vale notar que o SNR medido até subiu um pouco, o que mostra que essa métrica
sozinha não captura distorção. O arquivo ruim está guardado em
`data/saida/testeLucas_sem_ruido.wav` justamente como contraexemplo.

**Voltando para OGG, se quiser mandar no WhatsApp:**

```
ffmpeg -i data/saida/lucas_pa48.wav -c:a libopus -b:a 48k -ac 1 -ar 48000 \
  -vbr on -application voip data/saida/testeLucas_limpo.ogg
```

Os nove segundos de áudio saem de 849 KB em WAV para 52 KB em Opus.

## Os filtros disponíveis

| flag                        | o que faz                                       | usa halo? | padrão |
| --------------------------- | ----------------------------------------------- | --------- | ------ |
| `--ganho G`                 | multiplica a amplitude por `G`                  | não       | 1.0    |
| `--threshold L`             | zera amostras com módulo abaixo de `L`          | não       | 0.0    |
| `--convolucao R`            | média móvel de raio `R` (passa-baixa)           | sim       | 0      |
| `--passa-alta R`            | original menos a média móvel de raio `R`        | sim       | 0      |
| `--eco ATRASO ATENUACAO`    | soma uma cópia atrasada e atenuada              | sim (só à esquerda) | — |
| `--normalizar [ALVO]`       | escala o áudio até o pico global virar `ALVO`   | não (usa Allreduce) | 0.99 |
| `--no-parallel`             | força o OpenMP a usar uma única thread          | —         | —      |
| `--verbose`                 | liga os logs por rank e por thread              | —         | desligado |

As flags podem ser combinadas livremente, e a ordem de aplicação é fixa no
código, independente da ordem em que aparecem na linha de comando: ganho,
threshold, convolução, passa-alta, eco, e por último a normalização. Pedir
convolução e passa-alta juntos é válido e aplica os dois em sequência, nessa
ordem, o que dá um passa-faixa.

O **ganho** é o filtro mais simples, puramente ponto a ponto: `--ganho 0.5`
reduz o volume à metade.

O **threshold** zera todas as amostras cuja amplitude esteja abaixo do limiar.
Ele funciona bem em áudio sintético, onde o ruído tem amplitude muito menor que
o sinal, mas em voz gravada ele precisa de cuidado, porque é um gate por amostra
e não por trecho: se o limiar chegar perto do piso de ruído, ele corta dentro da
forma de onda do sinal e adiciona distorção em vez de limpar. O exemplo do Lucas,
acima, mostra esse efeito medido.

A **convolução** liga a média móvel com aquele raio, isto é, o número de amostras
de cada lado que entram na média. Por exemplo, `--convolucao 8` calcula cada
amostra como a média das dezessete amostras ao redor dela. É este filtro que usa
a troca de halo entre os blocos, e é ele que serve para suavizar chiado de alta
frequência, com o efeito colateral de abafar os agudos do sinal também.

O **passa-alta** faz o caminho contrário: em vez de guardar a parte grave do
sinal, ele subtrai essa parte grave e mantém a aguda, atenuando ruídos de baixa
frequência como ronco de vento ou zumbido de rede elétrica. O número é o mesmo
raio da média móvel, e vale notar que ele mexe na frequência de corte de um jeito
que à primeira vista parece invertido: um raio pequeno corta uma faixa mais larga
de graves, sendo mais agressivo, enquanto um raio grande corta só as frequências
mais baixas e preserva melhor o corpo do som. A atenuação que medimos para uma
taxa de 48000 Hz, em decibéis, é esta:

| raio | 60 Hz | 100 Hz | 200 Hz | 300 Hz | 500 Hz | 1 kHz |
| ---- | ----- | ------ | ------ | ------ | ------ | ----- |
| 8    | −62,6 | −53,7  | −41,7  | −34,7  | −25,9  | −14,3 |
| 16   | −51,1 | −42,2  | −30,2  | −23,3  | −14,7  | −4,2  |
| 32   | −39,3 | −30,5  | −18,7  | −12,0  | −4,4   | +1,7  |
| 48   | −32,4 | −23,6  | −12,1  | −6,0   | +0,1   | −0,1  |
| 64   | −27,5 | −18,8  | −7,7   | −2,2   | +1,6   | −0,9  |

Para voz, raios entre 32 e 64 costumam ser a faixa útil. Vale registrar que a
média móvel é um filtro bem cru, com stopband pobre, então parte do ronco
sobrevive em qualquer raio.

O **eco** recebe dois números: o atraso, em número de amostras, e a atenuação do
eco, entre zero e um. Cada amostra de saída passa a ser ela mesma somada a uma
cópia mais fraca de uma amostra anterior, o que dá a impressão de um som
repetindo com um pequeno retardo. Por exemplo, `--eco 4410 0.5` cria um eco de um
décimo de segundo, para um áudio de 44100 Hz, com metade do volume. Este filtro
também depende de amostras vizinhas, mas só das que vêm antes, então a troca de
halo é feita para o lado esquerdo, usando o atraso como tamanho da margem; por
isso o atraso não pode ser maior que o menor bloco, valendo a mesma restrição dos
outros filtros de vizinhança.

A **normalização** ajusta o volume do áudio para que o seu pico fique em um valor
alvo, aproveitando toda a faixa dinâmica sem estourar. Ela pode ser usada
sozinha, assumindo um alvo padrão de 0,99, ou seguida de um número que define o
alvo, como `--normalizar 0.95`. Este filtro é o mais interessante do ponto de
vista de paralelismo, porque precisa de uma informação global: o maior pico do
áudio inteiro, que está espalhado entre os blocos dos vários processos. Cada
processo calcula o pico do seu bloco e, com uma redução global (`MPI_Allreduce`
com a operação de máximo), todos passam a conhecer o mesmo pico global e aplicam
exatamente o mesmo fator de escala. Como a normalização roda depois dos demais
filtros, ela leva em conta o efeito deles; um eco, por exemplo, pode aumentar o
pico, e a normalização acomoda isso.

O **`--no-parallel`** não é um filtro, e serve para forçar o OpenMP a usar uma
única thread, o que é útil para comparar o tempo com e sem o paralelismo interno
dos blocos. Combinado com `-np 1`, ele dá a linha de base serial.

O **`--verbose`** também não é um filtro: ele liga os logs por rank e por thread,
descritos na seção sobre os logs. Ele infla os tempos, então não deve ser usado
em medições.

## Áudios de entrada e de saída já prontos

### Entrada, em `data/entrada/`

| arquivo | formato | duração | o que é |
| ------- | ------- | ------- | ------- |
| `testeLucas.ogg` | Opus 48 kHz mono | 9,07 s | mensagem de voz original, como chegou pelo WhatsApp |
| `testeLucas.wav` | PCM 16 bits 48 kHz mono | 9,05 s | a mesma mensagem já convertida, pronta para o programa. **É a entrada da demonstração principal** |
| `curto.wav` | PCM 16 bits 44,1 kHz mono | 0,50 s | seno sintético com ruído, gerado pelo script |
| `medio.wav` | PCM 16 bits 44,1 kHz mono | 5,00 s | idem, tamanho médio |
| `longo.wav` | PCM 16 bits 44,1 kHz mono | 30,00 s | idem; **é a entrada padrão do `benchmark.sh`** |
| `sanidade.wav` | PCM 16 bits 44,1 kHz mono | 1,00 s | gerado pelo `make test` |
| `Audio_com_Ruido1.wav` | PCM 16 bits 44,1 kHz estéreo | 15,41 s | gravação com ruído de vento, em estéreo |
| `Audio_com_Ruido2.wav` | PCM 16 bits 44,1 kHz estéreo | 28,39 s | idem, mais longa |
| `WhatsApp Ptt 2026-07-24 at 14.34.07.ogg` | Opus 48 kHz mono | 12,12 s | outra mensagem de voz, precisa converter antes de usar |
| `poquenaotlabaia_RV66pvOZ.mp3` | MP3 44,1 kHz estéreo | 4,31 s | trecho de música; **o programa não lê MP3**, precisa converter antes |

Os arquivos `curto`, `medio` e `longo` são sintéticos e podem ser regerados a
qualquer momento com o script em Python, que usa apenas a biblioteca padrão:

```
python3 scripts/gerar_wav_teste.py --preset
```

Também é possível gerar um arquivo específico ajustando a duração, a taxa de
amostragem, a frequência do seno, a amplitude e a quantidade de ruído:

```
python3 scripts/gerar_wav_teste.py --duracao 2.0 --freq 440 --ruido 0.05 --saida data/entrada/meu.wav
```

### Saída, em `data/saida/`

A pasta guarda só o conjunto da demonstração principal, todos os cinco gerados a
partir do `testeLucas.wav`:

| arquivo | como foi gerado |
| ------- | --------------- |
| `lucas_pa48.wav` + `lucas_pa48.csv` | `--passa-alta 48 --normalizar 0.95` — **o melhor resultado** |
| `lucas_pa32.wav` + `lucas_pa32.csv` | `--passa-alta 32 --normalizar 0.95`, mais agressivo nos graves |
| `lucas_pa64.wav` + `lucas_pa64.csv` | `--passa-alta 64 --normalizar 0.95`, mais conservador |
| `testeLucas_limpo.ogg` | o `lucas_pa48.wav` reconvertido para Opus, pronto para enviar |
| `testeLucas_sem_ruido.wav` + `testeLucas_metrica.csv` | `--threshold 0.01 --normalizar` — guardado como contraexemplo do que não fazer |

Os três primeiros dão para comparar de ouvido o efeito do raio, e o último serve
para ouvir a distorção que o gate por amostra introduz. Todos podem ser regerados
com os comandos da seção do exemplo completo.

As saídas de sanidade e de corretude não ficam guardadas de propósito: o
`benchmark.sh` escreve as dele em um diretório temporário e apaga no fim, para que
a pasta não acumule arquivos sem interesse sonoro. O `make test` é a exceção e
grava `sanidade_saida.wav` e `sanidade_metrica.csv` aqui.

## Convertendo formatos com ffmpeg

Como o programa só lê WAV PCM de 16 bits, qualquer áudio que venha de MP3, OGG,
M4A ou de um WAV de 24 ou 32 bits precisa passar pelo `ffmpeg` antes. O que
importa no comando é o `-c:a pcm_s16le`, que é justamente o formato aceito por
`src/wav_io.c`; sem ele o ffmpeg pode gerar ponto flutuante ou outro codec, e o
programa recusa o arquivo:

```
ffmpeg -i entrada.ogg -c:a pcm_s16le saida.wav
```

A taxa de amostragem é lida do cabeçalho, então não é obrigatório reamostrar.
Ainda assim, se for comparar métricas entre áudios diferentes, é mais confortável
deixar todos na mesma taxa, o que se faz com `-ar 44100`. Para forçar mono, use
`-ac 1`.

Para voltar de WAV para OGG com Opus, que é o formato que o WhatsApp usa:

```
ffmpeg -i saida.wav -c:a libopus -b:a 48k -ac 1 -ar 48000 -vbr on \
  -application voip saida.ogg
```

Aqui o `-c:a libopus` é o que importa, porque sem ele o ffmpeg gera Vorbis dentro
do OGG. O `-application voip` otimiza para voz, e o bitrate de 48k é confortável
para fala; para música vale subir para 96k ou mais.

Para inspecionar o formato de um arquivo antes de processar:

```
ffprobe -v error -show_entries stream=codec_name,sample_rate,channels,bits_per_sample entrada.wav
```

## Como medir: `scripts/benchmark.sh`

Os números de um relatório precisam poder ser refeitos por quem lê, então a
medição não é feita à mão. O script `scripts/benchmark.sh` roda a bateria inteira
com um comando e grava o resultado em `docs/resultados.md` (tabela pronta, com o
ambiente em que foi medida) e em `docs/resultados.csv` (dados crus, com mínimo e
máximo de cada configuração, não só a mediana):

```
make bench             # corretude + varredura de desempenho
make check             # só a bateria de corretude, mais rápido
```

Ou diretamente, para controlar a varredura:

```
scripts/benchmark.sh --audio data/entrada/longo.wav --reps 9
scripts/benchmark.sh --np "1 2 4" --threads "1 4"
scripts/benchmark.sh --hostfile maquinas.txt      # execução multi-nó
```

A parte de corretude tem dezoito verificações, e a maioria delas é do mesmo tipo:
processar o mesmo áudio com números diferentes de processos e exigir que a saída
seja **byte a byte idêntica**. Isso vale para a identidade sem filtros, para cada
filtro de vizinhança separadamente (que só casam se o halo estiver correto), para
a cadeia completa com normalização, e para o caso-limite em que o raio do filtro é
exatamente igual ao menor bloco. Há também a verificação de que as métricas não
mudam com o número de threads, que existe porque a FFT tem três caminhos de código
diferentes conforme quantas threads existem, e todos precisam concordar. O script
sai com código de erro se qualquer uma falhar, e avisa que os tempos não
significam nada quando há falha de corretude.

Duas decisões de medição estão embutidas no script e valem explicação:

- **Todas as execuções usam `--bind-to none`.** Por padrão o OpenMPI amarra cada
  processo a um núcleo, o que prende todas as threads OpenMP daquele rank no mesmo
  núcleo e faz o OpenMP parecer inútil. A diferença é grosseira: medido neste
  projeto, com `--bind-to core` o tempo fica travado em torno de 0,14 s de uma a
  oito threads, enquanto com `--bind-to none` cai de 0,20 s para 0,06 s. Sem essa
  flag, uma varredura de threads mede a afinidade de CPU, não o código.
- **Nenhuma execução usa `--verbose`,** pelo motivo já explicado.

Além disso o script reporta a **mediana** de várias repetições, e não o melhor
tempo. O melhor de N é otimista por construção e esconde variância; nas nossas
medições a diferença entre execuções da mesma configuração chegou a 27%, o que é
maior que vários dos efeitos que se quer medir. As linhas em que processos vezes
threads passa do número de núcleos ficam marcadas com `*`, porque nessas há
disputa por CPU e o número não é comparável com as outras.

## Sobre os testes que fizemos

A corretude está toda automatizada em `make check`, descrito na seção anterior, e
a ideia por trás dela é sempre a mesma: o resultado não pode depender de como o
trabalho foi dividido. Verificamos que a saída sai idêntica à entrada quando
nenhum filtro é aplicado, comparando byte a byte, inclusive em tamanhos que não
dividem igual entre os processos; que cada filtro de vizinhança produz o mesmo
arquivo com um, quatro e oito processos, o que só acontece se o halo estiver
correto; que a cadeia completa de convolução, eco e normalização também é
invariante, o que exercita a redução global; que as métricas não mudam com o número
de threads, verificação que existe porque a FFT tem três caminhos de código
diferentes conforme quantas threads existem; e os dois lados do caso-limite do
halo, em um áudio de 44 amostras onde com quatro processos o menor bloco é
exatamente 11: raio 11 tem que casar com a execução de um processo, e raio 12 tem
que ser **recusado com erro**, não dar um resultado errado em silêncio. Na última
execução, as dezoito verificações passaram.

Duas conferências numéricas complementam isso, e ambas conferem. O filtro de ganho
escala a energia na proporção exata: sobre o `curto.wav`, o RMS de cada bloco cai
de 0,5683 para 0,2842 com `--ganho 0.5`, uma razão de 0,5000 em todos os quatro
blocos. E a frequência dominante calculada pela nossa transformada dá 441,43 Hz
para um seno gerado em 440 Hz, o que fica dentro de um bin da FFT naquele tamanho
de bloco.

Quanto ao desempenho, a tabela de referência não é escrita à mão: ela é gerada
por `make bench` e fica em `docs/resultados.md`, junto com o ambiente em que foi
medida. O resumo da medição atual, sobre o `longo.wav` com `--passa-alta 48`,
mediana de cinco repetições em 28 núcleos, com a base sequencial (`-np 1
--no-parallel`) em 0,109 s:

| configuração | t_total | speedup |
| ------------ | ------- | ------- |
| np=1, threads=1 | 0,130 s | 1,00× (referência da tabela) |
| np=1, threads=8 | 0,046 s | 2,81× |
| np=4, threads=4 | 0,027 s | 4,91× |
| **np=8, threads=1** | **0,020 s** | **6,52×** |

Os dois níveis de paralelismo ajudam, e o MPI ajuda mais que o OpenMP: sozinho, o
OpenMP tira 2,81× com oito threads em um processo, enquanto oito processos de uma
thread tiram 6,52×. Isso é coerente com a estrutura do programa, em que parte do
custo por rank está fora dos laços paralelizados — em `filtrar_com_vizinhanca`,
cada chamada faz duas alocações, a cópia do halo e um `memcpy` de volta, tudo
proporcional ao tamanho do bloco e tudo serial dentro do processo. O MPI reduz
esse tamanho dividindo o áudio; o OpenMP não. É uma leitura sustentada pelas
medições, não algo confirmado com profiler.

Duas armadilhas que encontramos pelo caminho valem registro, porque as duas fazem
o paralelismo parecer inútil sem que haja nada de errado com o código:

A primeira é a **amarração de CPU do `mpirun`**. Com o padrão `--bind-to core`,
todas as threads de um rank ficam presas em um núcleo só e o tempo trava em torno
de 0,14 s independentemente de quantas threads se peça; com `--bind-to none` ele
cai de 0,20 s para 0,06 s na mesma varredura. Sem essa flag, uma varredura de
threads mede afinidade de CPU, não o código, e é por isso que o `benchmark.sh` a
usa em todas as execuções.

A segunda é a **oversubscrição de threads**, que aparecia quando cada processo
abria uma thread por núcleo: quatro processos em 28 núcleos davam 112 threads, e
as barreiras da FFT tropeçavam no escalonador a ponto de o tempo *piorar* ao
adicionar processos. Isso motivou duas mudanças no código, e as duas continuam
valendo a pena entender: o programa passou a repartir os núcleos entre os
processos do mesmo nó quando `OMP_NUM_THREADS` não está definido, e a FFT passou a
abrir uma única região paralela em vez de uma por etapa. Com isso o tempo voltou a
cair de forma monótona e não é mais preciso ajustar `OMP_NUM_THREADS` à mão. O
efeito residual ainda é visível nas linhas marcadas com `*` em
`docs/resultados.md`, que são as configurações em que processos vezes threads
passa dos 28 núcleos.

As anotações mais detalhadas desses testes estão em `docs/relatorio.md`, e os
dados crus de cada configuração, com mínimo e máximo além da mediana, em
`docs/resultados.csv`.

## Integrantes do grupo

[nomes dos integrantes]
