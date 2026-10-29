# Palhetador Automático de Guitarra com Arduino e Motor de Passo

Sistema embarcado para testes automatizados de palhetada contínua (*alternate picking*) em cordas de guitarra ou violão, utilizando **Arduino Uno Rev3**, display **DFRobot LCD Keypad Shield** e motor de passo com redução **12V 32-Step (Adafruit 918 / 28BYJ)**.

---

## 1. Visão Geral da Arquitetura

O sistema aciona uma haste horizontal fixada ao eixo do motor, na extremidade da qual fica presa uma palheta. A haste oscila para frente e para trás entre $-\text{Amplitude}$ e $+\text{Amplitude}$ em torno de um ponto central calibrado.

### Principais Recursos
* **Controle de Amplitude em Tempo Real:** Regulagem precisa do ângulo de ataque/curso da palheta através das teclas `UP` e `DOWN`, mesmo durante a execução.
* **Controle de Velocidade / Frequência:** Ajuste de 10 níveis de velocidade pelas teclas `LEFT` e `RIGHT`, com cálculo instantâneo de **SPM** (*Strokes Per Minute* / Palhetadas Por Minuto).
* **Modo de Alinhamento / Calibração (*Jog Center*):** Permite mover a haste passo a passo via botões para posicionar a palheta exatamente sobre a corda desejada antes de travar o ponto zero, **evitando forçar manualmente a caixa de engrenagens do motor**.
* **Arquitetura 100% Não-Bloqueante:** O motor é acionado via temporização em microssegundos (`micros()`), garantindo resposta instantânea aos botões e sem travar a interface.
* **Gerenciamento Térmico (*Coil Release*):** Ao pausar (`STOP`), todas as bobinas são desenergizadas automaticamente, impedindo aquecimento desnecessário.
* **Acionamento em Meio-Passo (*Half-Stepping*):** Sequência de 8 fases que proporciona menor vibração mecânica, movimento 100% simétrico e máxima suavidade no contato com as cordas.

---

## 2. Hardware e Pinagem

### Componentes Utilizados
1. **Arduino Uno Rev3**
2. **DFRobot LCD Keypad Shield** (Display 16x2 com teclado analógico integrado em `A0`)
3. **Motor de Passo 28BYJ-48 5V** (Unipolar 5 fios com redução ~1:64)
4. **Módulo Driver ULN2003**
5. **Alimentação:** Cabo USB conectado ao computador (5V)

### Mapa de Pinos do Arduino Uno

| Pino Arduino | Conexão / Função | Observação |
| :--- | :--- | :--- |
| **D4, D5, D6, D7** | Linhas de Dados LCD (DB4-DB7) | Utilizado internamente pelo Shield |
| **D8** | LCD RS (Register Select) | Utilizado internamente pelo Shield |
| **D9** | LCD Enable | Utilizado internamente pelo Shield |
| **D10** | LCD Backlight Control | Utilizado internamente pelo Shield |
| **A0** | Teclado Analógico (5 botões) | Divisor resistivo (Right, Up, Down, Left, Select) |
| **A1** | **Driver ULN2003 - IN1** | Saída digital livre no Shield (conector sequencial) |
| **A2** | **Driver ULN2003 - IN2** | Saída digital livre no Shield (conector sequencial) |
| **A3** | **Driver ULN2003 - IN3** | Saída digital livre no Shield (conector sequencial) |
| **A4** | **Driver ULN2003 - IN4** | Saída digital livre no Shield (conector sequencial) |
| **5V** | **Alimentação ULN2003 (+)** | Para motor 28BYJ-48 5V alimentado pela USB |
| **GND** | **GND Comum (-)** | Conectar ao (-) do ULN2003 |

---

## 3. Esquema de Ligação (Alimentação USB 5V)

```
  +------------------+                    +-------------------------+
  |  Arduino Uno     |--- (5V) ---------->| (+) 5-12V               |
  |  + DFRobot LCD   |--- (GND) --------->| (-) GND     ULN2003     |
  |                  |                    |                         |
  |                  |                    |     Conector 5 pinos    |==== [Motor 28BYJ-48 5V]
  |                  |                    |      (Azul, Rosa,       |
  |                  |                    |    Amarelo, Laranja,    |
  |                  |                    |        Vermelho)        |
  |                  |                    |                         |
  |                  |                    | IN1  IN2   IN3   IN4    |
  |                  |                    +---|----|-----|-----|----+
  |               A1 |------------------------+    |     |     |
  |               A2 |-----------------------------+     |     |
  |               A3 |-----------------------------------+     |
  |               A4 |-----------------------------------------+
  +------------------+
```

---

## 4. Cálculos Mecânicos: Amplitude Angular e Linear

O motor **28BYJ-48 5V** opera em modo **Meio-Passo (*Half-Stepping* de 8 Fases)** para rotação 100% suave, simétrica e sem perda de sincronismo:

* **Meio-passos por volta completa (360°):** $4096\text{ meio-passos/revolução}$
* **Resolução angular por meio-passo:**
  $$\theta_{\text{passo}} = \frac{360^\circ}{4096} \approx 0.08789^\circ$$

### Como calcular o curso linear na ponta da palheta:
Se a haste horizontal tiver comprimento $L$ (em milímetros, medido do centro do eixo do motor até a ponta da palheta) e a amplitude configurada for de $\pm A$ passos:

1. **Ângulo total de excursão ($\Delta\theta$):**
   $$\Delta\theta = 2 \times A \times 0.08789^\circ$$
2. **Deslocamento linear total do arco ($\Delta s$):**
   $$\Delta s \approx L \times \left(\Delta\theta \times \frac{\pi}{180^\circ}\right)$$

#### Exemplo Prático:
Para uma haste de $L = 50\text{ mm}$ com amplitude padrão de $A = 100\text{ passos}$ ($\pm 8.8^\circ$, curso total de $17.6^\circ$):
$$\Delta s \approx 50\text{ mm} \times \left(17.6^\circ \times \frac{\pi}{180}\right) \approx 15.3\text{ mm}$$
*(Ideal para palhetar com firmeza a corda do instrumento).*

---

## 5. Como Operar o Sistema

O display LCD de 16x2 fornece leitura contínua e status do sistema:

### Telas do Display

#### 1. Modo Normal (`[RUN]` ou `[STOP]`)
```
+----------------+
|[RUN]  V:5 100SPM| -> Linha 0: Status, Nível de Vel (1-10) e Frequência (SPM)
|A:+-100p  8.8°  | -> Linha 1: Amplitude em passos e graus equivalentes
+----------------+
```

#### 2. Modo de Alinhamento Manual (`[AJUSTE CENTRO]`)
```
+----------------+
|[AJUSTE CENTRO] | -> Modo de ajuste da posição de repouso
|Pos:  +4  UP/DWN| -> Posição de deslocamento atual
+----------------+
```

### Funções dos Botões

| Botão | No Modo `RUN` | No Modo `STOP` | No Modo `AJUSTE CENTRO` |
| :--- | :--- | :--- | :--- |
| **SELECT** (Clique Curto) | Para o motor e volta ao centro (`STOP`) | Inicia a oscilação da palheta (`RUN`) | — |
| **SELECT** (Segurar 1.5s) | — | Entra no modo de **Ajuste de Centro** | Salva o novo centro e sai para `STOP` |
| **UP** | Aumenta a amplitude (+4 passos) | Aumenta a amplitude (+4 passos) | Avanço rápido (CW) para alinhar palheta |
| **DOWN** | Diminui a amplitude (-4 passos) | Diminui a amplitude (-4 passos) | Avanço rápido (CCW) para alinhar palheta |
| **RIGHT** | Aumenta velocidade (níveis 1 a 10) | Aumenta velocidade | — |
| **LEFT** | Diminui velocidade | Diminui velocidade | — |

---

### Controle Alternativo via Serial (USB)
Caso os botões físicos do shield apresentem mau contato ou oxidação, é possível operar o sistema com 100% de estabilidade pelo teclado do computador via Monitor Serial (115200 baud):

| Tecla | Função |
| :---: | :--- |
| **`Espaço`** ou **`r`** | Alterna entre `RUN` e `STOP` |
| **`+`** / **`-`** | Aumenta / Diminui a Amplitude de palhetada |
| **`1`** a **`9`** | Seleciona o nível de velocidade |
| **`a`** | Entra / Sai do modo de **Ajuste de Centro** |
| **`u`** / **`d`** | Move para cima / baixo no alinhamento |
| **`z`** | Salva a posição atual como o novo ponto ZERO central |
| **`b`** | Ativa / Desativa a leitura dos botões físicos do shield (elimina interferências) |
| **`?`** | Exibe o menu de ajuda no terminal |

> [!TIP]
> **Como alinhar a palheta na primeira montagem:**
> 1. Encaixe a haste no eixo do motor sem forçar a rotação.
> 2. Ligue o Arduino (ele iniciará em `[STOP]`).
> 3. Segure o botão **`SELECT`** por mais de 1 segundo até aparecer `[AJUSTE CENTRO]`.
> 4. Use **`UP`** ou **`DOWN`** para mover suavemente a palheta até que ela fique exatamente centralizada sobre a corda da guitarra.
> 5. Pressione **`SELECT`** brevemente. A posição atual é gravada como o **ponto zero de repouso**.
> 6. Pressione **`SELECT`** novamente para iniciar o teste de palhetada!

---

## 6. Dicas para Testes com Cordas de Instrumento

1. **Flexibilidade da Palheta:** Use palhetas médias ou finas (0.60mm a 0.88mm) para os primeiros testes. Palhetas excessivamente rígidas (acima de 1.5mm) podem transferir choque mecânico excessivo ao redutor do motor caso o curso esteja muito profundo.
2. **Ângulo de Ataque:** Fixe a palheta com uma leve inclinação angular (~5° a 10° em relação à vertical da corda) para que ela deslize suavemente pela corda na ida e na volta, simulando a técnica natural de palhetada alternada humana.
3. **Ajuste Fino da Amplitude:** Inicie o teste com amplitude baixa (ex: $\pm 10$ passos) com o motor posicionado próximo à corda e aumente gradualmente usando o botão `UP` até ouvir o som limpo do ataque da palheta.
