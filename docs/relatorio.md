# Anotações de testes e resultados

Estas são anotações de trabalho para apoiar a escrita do README e a
apresentação. Registram como os testes foram feitos e os tempos medidos.

## Ambiente de teste

Máquina de desenvolvimento com 28 núcleos, OpenMPI 4.1.2, gcc 11.4. Os tempos
abaixo são do ambiente local; na Xivoco os números absolutos mudam, mas a
tendência (mais processos, menos tempo até certo ponto) deve se manter.

Os áudios de teste foram gerados com `scripts/gerar_wav_teste.py --preset`:
curto (0,5 s, 22050 amostras), médio (5 s, 220500 amostras) e longo
(30 s, 1323000 amostras), todos mono 16 bits a 44100 Hz. Os tamanhos foram
escolhidos para nem sempre dividirem igual entre os processos, forçando o uso
de Scatterv com blocos de tamanhos diferentes.

## Correção

O primeiro teste foi rodar com ganho 1.0 (identidade). Nesse caso a saída tem
que sair byte a byte igual à entrada, o que confirma que a malha de
comunicação Scatterv/Gatherv não corrompe nem embaralha as amostras. Conferido
com `cmp` para np = 1 e np = 4 (este último com blocos desiguais: 5513, 5513,
5512, 5512 para 22050 amostras). Ambos idênticos à entrada.

Em seguida, com ganho 0.5, a energia RMS caiu de 0.5674 para 0.2837 (metade,
como esperado). O resultado com np = 1 e np = 4 é idêntico byte a byte, ou seja,
o número de processos não altera o resultado, só a forma de calcular.

A frequência dominante calculada pela FFT bate com o conteúdo dos áudios: o
áudio longo, gerado com um seno de 1000 Hz, resultou em 999,95 Hz no CSV; o
curto e o médio, gerados com 440 Hz, ficaram em torno de 438-441 Hz (a
resolução depende do tamanho da janela de FFT, que varia com o tamanho do
bloco).

## Tempos por número de processos

Tempo de parede da região paralela (do Bcast até o Gather, medido com
MPI_Wtime no rank 0), menor de cinco execuções, com OMP_NUM_THREADS = 2.
Não inclui a leitura e a escrita em disco, que são feitas só pelo rank 0.

Áudio médio (220500 amostras):

    np = 1 -> 0.0151 s
    np = 2 -> 0.0088 s
    np = 4 -> 0.0053 s
    np = 8 -> 0.0033 s

Áudio longo (1323000 amostras):

    np = 1 -> 0.1025 s
    np = 2 -> 0.0701 s
    np = 4 -> 0.0424 s
    np = 8 -> 0.0207 s

No áudio longo o ganho de np = 1 para np = 8 é de cerca de 5 vezes. O
escalonamento não é perfeitamente linear porque parte do tempo é gasto na
comunicação (espalhar e recolher os blocos) e essa parte não diminui quando se
adiciona mais processos.

## Efeito do OpenMP isolado

Fixando np = 4 e variando o número de threads OpenMP, no áudio longo:

    OMP = 1 -> 0.0412 s
    OMP = 2 -> 0.0402 s
    OMP = 4 -> 0.0395 s
    OMP = 8 -> 0.0369 s

O ganho do OpenMP aqui é pequeno. Isso faz sentido para esta carga: os filtros
ponto a ponto (ganho e threshold) são operações muito baratas por amostra,
então o laço já é rápido e o tempo acaba dominado pela parte serial de cada
bloco (o cálculo da FFT para a métrica de frequência dominante, que na versão
atual roda em uma thread só). A maior parte do ganho de desempenho vem da
decomposição por MPI, não do OpenMP. Vale registrar isso com honestidade no
relatório em vez de forçar um número bonito.

Como referência sequencial, rodar com np = 1 e a flag --no-parallel (uma única
thread) no áudio longo deu 0.1341 s.

## Filtro por convolução com halo

Como passo extra, implementamos um filtro de média móvel (um passa-baixa), que
depende de amostras vizinhas. Antes de aplicar, cada processo troca com os
vizinhos uma margem de amostras (halo) usando MPI_Sendrecv, e as pontas globais
são preenchidas com zero. O teste de correção foi comparar a saída com números
de processos diferentes: com raio 8 no áudio médio, a saída de np = 1, np = 4 e
np = 8 é byte a byte idêntica, o que só acontece se o halo estiver sendo trocado
corretamente. Também comparamos com uma média móvel de referência calculada em
Python sobre o áudio inteiro, e a diferença máxima foi zero. O filtro é ativado
pela opção --convolucao seguida do raio.

A partir da mesma média móvel montamos também um passa-alta, ativado por
--passa-alta seguido do raio. A ideia é que a média móvel guarda a parte grave
do sinal; subtraindo essa média do valor original, sobra a parte aguda. Serve
para atenuar ruído de baixa frequência, como o ronco de vento. Testamos com um
seno de 100 Hz e um de 8000 Hz (sem ruído). Com raio 16, o RMS do seno de 100 Hz
caiu de 0.5657 para 0.0052, quase sumindo, enquanto o de 8000 Hz ficou
praticamente igual (0.5657 para 0.5670). Vale registrar um detalhe que parece
contraintuitivo: o raio menor é mais agressivo nos graves. Com raio 64 o mesmo
seno de 100 Hz caiu só para 0.0763. Isso acontece porque o passa-alta é o
complemento da média móvel, então uma janela maior (que suaviza mais) já começa
a deixar passar parte do grave. Assim como o passa-baixa, a saída do passa-alta é
idêntica com np = 1 e np = 4 e casa exatamente com uma referência calculada em
Python.

## Observações para a Xivoco

O binário compila com `make` e roda com
`mpirun -np <N> ./processa_audio entrada.wav saida.wav metrica.csv`. Como os
workers da Xivoco não compartilham disco, é importante lembrar que só o rank 0
lê e escreve arquivo; se por acidente outro rank tentasse abrir o arquivo, ele
simplesmente não estaria lá. O caminho de entrada precisa existir na máquina
onde o rank 0 roda.
