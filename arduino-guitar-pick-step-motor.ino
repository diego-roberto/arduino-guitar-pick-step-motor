/*
  ==============================================================================
  PROJETO: Palhetador Automático de Guitarra com Motor de Passo
  HARDWARE:
    - Arduino Uno Rev3
    - DFRobot LCD Keypad Shield (16x2 + Teclado Analógico em A0)
    - Motor de Passo: Small Reduction Stepper Motor - 12VDC 32-Step (Adafruit 918)
    - Driver: Módulo ULN2003 (ou ponte H equivalente para motor unipolar 5 vias)
  ==============================================================================
*/

#include <LiquidCrystal.h>

// ==============================================================================
// DEFINIÇÕES DE PINOS
// ==============================================================================

// Pinos padrão do DFRobot LCD Keypad Shield
// RS, EN, D4, D5, D6, D7
LiquidCrystal lcd(8, 9, 4, 5, 6, 7);

const uint8_t PIN_KEYPAD     = A0; // Leitura dos botões analógicos
const uint8_t PIN_BACKLIGHT  = 10; // Controle de brilho/ligar backlight

// Pinos livres no Shield DFRobot para acionamento do Driver ULN2003
// (Conecte aos terminais IN1, IN2, IN3, IN4 da placa ULN2003)
const uint8_t PIN_IN1        = 2;
const uint8_t PIN_IN2        = 3;
const uint8_t PIN_IN3        = 11;
const uint8_t PIN_IN4        = 12;

// ==============================================================================
// PARÂMETROS DO MOTOR DE PASSO
// Motor: 32 passos nativos * redução ~1/16 (exatamente 16 ou 16.032)
// Em modo meio-passo (Half-Step / 8 fases): 32 * 16 * 2 = 1024 meio-passos / volta
// ==============================================================================
const float STEPS_PER_REV    = 1024.0f; // Meio-passos para 360°
const float DEG_PER_STEP     = 360.0f / STEPS_PER_REV; // ~0.3516° por meio-passo

// Tabela de sequência em meio-passo (Half-stepping: 8 estados)
// Proporciona menor vibração, maior suavidade e melhor torque para palhetar
const uint8_t STEP_SEQ[8][4] = {
  {HIGH, LOW,  LOW,  LOW }, // 1: A
  {HIGH, HIGH, LOW,  LOW }, // 2: A+B
  {LOW,  HIGH, LOW,  LOW }, // 3: B
  {LOW,  HIGH, HIGH, LOW }, // 4: B+C
  {LOW,  LOW,  HIGH, LOW }, // 5: C
  {LOW,  LOW,  HIGH, HIGH}, // 6: C+D
  {LOW,  LOW,  LOW,  HIGH}, // 7: D
  {HIGH, LOW,  LOW,  HIGH}  // 8: D+A
};

// ==============================================================================
// MAPEAMENTO DOS BOTÕES DO SHIELD
// ==============================================================================
enum Button {
  BTN_NONE,
  BTN_RIGHT,
  BTN_UP,
  BTN_DOWN,
  BTN_LEFT,
  BTN_SELECT
};

// ==============================================================================
// ESTADOS DA MÁQUINA
// ==============================================================================
enum SystemState {
  STATE_STOP,   // Parado no ponto zero, bobinas desenergizadas (não esquenta)
  STATE_RUN,    // Palhetando alternadamente (-Amp <-> +Amp)
  STATE_ALIGN   // Modo de ajuste/calibração manual do centro
};

SystemState currentState = STATE_STOP;

// ==============================================================================
// VARIÁVEIS DE CONTROLE DO MOVIMENTO
// ==============================================================================
int amplitudeSteps = 30;     // Amplitude em meio-passos (±30 passos ≈ ±10.5°, curso total de 21°)
int minAmplitude   = 4;      // Mínimo de passos
int maxAmplitude   = 200;    // Máximo de passos (±70°)

int speedLevel     = 5;      // Nível de velocidade (1 a 10)
const int MIN_SPEED = 1;
const int MAX_SPEED = 10;

// Mapeamento de velocidade (1 = lento / 6000µs por passo; 10 = rápido / 1500µs por passo)
const unsigned int SPEED_DELAY_US[] = {
  0,     // índice 0 não usado
  6000,  // Vel 1 (Muito suave)
  5000,  // Vel 2
  4200,  // Vel 3
  3500,  // Vel 4
  2800,  // Vel 5 (Padrão)
  2300,  // Vel 6
  1900,  // Vel 7
  1600,  // Vel 8
  1400,  // Vel 9
  1200   // Vel 10 (Máxima velocidade segura a 12V)
};

// Posição angular atual (em passos relativos ao centro 0)
long currentStepPos = 0;
int  motionDirection = 1;    // +1 = avançando para +Amp; -1 = voltando para -Amp
uint8_t stepPhaseIndex = 0;  // Índice na tabela de sequência (0 a 7)

// Temporizadores (não bloqueantes)
unsigned long lastStepMicros = 0;
unsigned long lastLcdUpdate   = 0;
const unsigned long LCD_REFRESH_INTERVAL_MS = 120; // Atualiza LCD a cada 120ms sem travar o motor

// Detecção e debounce de botões
Button lastDetectedBtn = BTN_NONE;
unsigned long btnPressStartTime = 0;
unsigned long lastBtnRepeatTime = 0;
bool selectHandledLongPress = false;

// ==============================================================================
// FUNÇÕES DE ACIONAMENTO DO MOTOR
// ==============================================================================

// Aplica a fase correspondente nos pinos do motor
void applyStepPhase(uint8_t phase) {
  digitalWrite(PIN_IN1, STEP_SEQ[phase][0]);
  digitalWrite(PIN_IN2, STEP_SEQ[phase][1]);
  digitalWrite(PIN_IN3, STEP_SEQ[phase][2]);
  digitalWrite(PIN_IN4, STEP_SEQ[phase][3]);
}

// Desliga todas as bobinas do motor para economizar energia e evitar aquecimento
void releaseMotor() {
  digitalWrite(PIN_IN1, LOW);
  digitalWrite(PIN_IN2, LOW);
  digitalWrite(PIN_IN3, LOW);
  digitalWrite(PIN_IN4, LOW);
}

// Executa um único meio-passo na direção indicada (+1 ou -1)
void stepMotor(int dir) {
  if (dir > 0) {
    stepPhaseIndex = (stepPhaseIndex + 1) % 8;
    currentStepPos++;
  } else if (dir < 0) {
    stepPhaseIndex = (stepPhaseIndex + 7) % 8; // Equivalente a -1 com wrap-around
    currentStepPos--;
  }
  applyStepPhase(stepPhaseIndex);
}

// Move suavemente de volta ao centro zero
void returnToCenter() {
  while (currentStepPos != 0) {
    int dir = (currentStepPos > 0) ? -1 : 1;
    stepMotor(dir);
    delayMicroseconds(2000);
  }
  releaseMotor();
}

// ==============================================================================
// LEITURA E DEBOUNCE DO TECLADO ANALÓGICO
// ==============================================================================
Button readKeypadRaw() {
  int adcVal = analogRead(PIN_KEYPAD);
  // Faixas típicas do divisor de tensão do DFRobot Keypad Shield
  if (adcVal > 850) return BTN_NONE;
  if (adcVal < 60)  return BTN_RIGHT;
  if (adcVal < 220) return BTN_UP;
  if (adcVal < 395) return BTN_DOWN;
  if (adcVal < 600) return BTN_LEFT;
  if (adcVal < 820) return BTN_SELECT;
  return BTN_NONE;
}

// ==============================================================================
// ATUALIZAÇÃO DO DISPLAY LCD (16x2)
// ==============================================================================
void updateDisplay(bool force = false) {
  unsigned long now = millis();
  if (!force && (now - lastLcdUpdate < LCD_REFRESH_INTERVAL_MS)) {
    return;
  }
  lastLcdUpdate = now;

  // Cálculo da frequência aproximada em Palhetadas Por Minuto (SPM - Strokes Per Minute)
  // Cada ciclo completo (ida e volta) possui 4 * amplitudeSteps meio-passos
  unsigned long delayUs = SPEED_DELAY_US[speedLevel];
  float cycleSeconds = (4.0f * (float)amplitudeSteps * (float)delayUs) / 1000000.0f;
  int spm = (cycleSeconds > 0) ? (int)(60.0f / cycleSeconds) : 0;

  // Cálculo da amplitude em graus (conversão para inteiro e decimal sem depender de %f no AVR)
  int degX10 = (int)(amplitudeSteps * DEG_PER_STEP * 10.0f + 0.5f);
  int degInt = degX10 / 10;
  int degDec = degX10 % 10;

  char line0[17];
  char line1[17];

  switch (currentState) {
    case STATE_RUN:
      // Linha 0: [RUN] V:5 180SPM
      snprintf(line0, sizeof(line0), "[RUN] V:%-2d %3dSPM", speedLevel, spm);
      // Linha 1: A:+-30p  10.5°
      snprintf(line1, sizeof(line1), "A:+-%-3dp %2d.%1d%c", amplitudeSteps, degInt, degDec, 0xDF);
      break;

    case STATE_STOP:
      // Linha 0: [STOP] V:5 180SPM
      snprintf(line0, sizeof(line0), "[STOP] V:%-2d%3dSPM", speedLevel, spm);
      // Linha 1: A:+-30p  10.5°
      snprintf(line1, sizeof(line1), "A:+-%-3dp %2d.%1d%c", amplitudeSteps, degInt, degDec, 0xDF);
      break;

    case STATE_ALIGN:
      // Modo de alinhamento fino manual da palheta sobre a corda
      snprintf(line0, sizeof(line0), "[AJUSTE CENTRO] ");
      snprintf(line1, sizeof(line1), "Pos:%+4ld  UP/DWN", currentStepPos);
      break;
  }

  // Escreve diretamente sem limpar tela inteira (evita flickering e atraso)
  lcd.setCursor(0, 0);
  lcd.print(line0);
  // Garante preenchimento de 16 caracteres
  for (int i = strlen(line0); i < 16; i++) lcd.print(' ');

  lcd.setCursor(0, 1);
  lcd.print(line1);
  for (int i = strlen(line1); i < 16; i++) lcd.print(' ');
}

// ==============================================================================
// PROCESSAMENTO DE ENTRADAS DO USUÁRIO
// ==============================================================================
void handleKeypad() {
  Button currentBtn = readKeypadRaw();
  unsigned long now = millis();

  // Reset se nenhum botão pressionado
  if (currentBtn == BTN_NONE) {
    if (lastDetectedBtn == BTN_SELECT && !selectHandledLongPress) {
      // Clique curto no SELECT: Alterna entre RUN e STOP
      if (currentState == STATE_RUN) {
        currentState = STATE_STOP;
        returnToCenter();
      } else if (currentState == STATE_STOP) {
        currentState = STATE_RUN;
        motionDirection = 1;
      }
      updateDisplay(true);
    }
    lastDetectedBtn = BTN_NONE;
    selectHandledLongPress = false;
    btnPressStartTime = 0;
    return;
  }

  // Novo botão detectado
  if (currentBtn != lastDetectedBtn) {
    lastDetectedBtn = currentBtn;
    btnPressStartTime = now;
    lastBtnRepeatTime = now;
    selectHandledLongPress = false;

    // Dispara ação no primeiro clique imediato (exceto SELECT, que tem verificação de clique longo)
    if (currentBtn != BTN_SELECT) {
      executeButtonAction(currentBtn);
      updateDisplay(true);
    }
    return;
  }

  // Botão continuou sendo pressionado: Trata clique longo do SELECT para entrar no modo ALIGN
  if (currentBtn == BTN_SELECT && !selectHandledLongPress) {
    if (now - btnPressStartTime >= 1200) { // Segurou SELECT por 1.2 segundos
      selectHandledLongPress = true;
      if (currentState == STATE_STOP) {
        currentState = STATE_ALIGN;
        currentStepPos = 0; // Ponto de partida do alinhamento
      } else if (currentState == STATE_ALIGN) {
        // Confirma a posição atual como o novo ZERO
        currentStepPos = 0;
        currentState = STATE_STOP;
        releaseMotor();
      }
      updateDisplay(true);
    }
    return;
  }

  // Repetição automática ao segurar botões de ajuste (UP, DOWN, LEFT, RIGHT)
  if (currentBtn != BTN_SELECT) {
    if (now - btnPressStartTime > 400 && now - lastBtnRepeatTime > 90) {
      lastBtnRepeatTime = now;
      executeButtonAction(currentBtn);
      updateDisplay(true);
    }
  }
}

// Executa a ação do botão de acordo com o estado do sistema
void executeButtonAction(Button btn) {
  if (currentState == STATE_ALIGN) {
    // No modo de alinhamento: UP move no sentido horário, DOWN move no anti-horário
    if (btn == BTN_UP) {
      stepMotor(1);
    } else if (btn == BTN_DOWN) {
      stepMotor(-1);
    }
    return;
  }

  // Nos modos STOP e RUN:
  // UP / DOWN: Ajusta amplitude do curso de palhetada
  if (btn == BTN_UP) {
    if (amplitudeSteps < maxAmplitude) {
      amplitudeSteps += 2; // Incrementa de 2 em 2 meio-passos (~0.7°)
    }
  } else if (btn == BTN_DOWN) {
    if (amplitudeSteps > minAmplitude) {
      amplitudeSteps -= 2;
    }
  }

  // RIGHT / LEFT: Ajusta nível de velocidade
  if (btn == BTN_RIGHT) {
    if (speedLevel < MAX_SPEED) speedLevel++;
  } else if (btn == BTN_LEFT) {
    if (speedLevel > MIN_SPEED) speedLevel--;
  }
}

// ==============================================================================
// SETUP
// ==============================================================================
void setup() {
  // Configuração dos pinos do motor como saída
  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  pinMode(PIN_IN3, OUTPUT);
  pinMode(PIN_IN4, OUTPUT);
  releaseMotor();

  // Configuração do Backlight do shield
  pinMode(PIN_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_BACKLIGHT, HIGH);

  // Inicialização do LCD
  lcd.begin(16, 2);
  lcd.clear();
  lcd.print("Palhetador v1.0");
  lcd.setCursor(0, 1);
  lcd.print("Iniciando...");
  delay(1200);

  lcd.clear();
  updateDisplay(true);
}

// ==============================================================================
// LOOP PRINCIPAL (100% NÃO-BLOQUEANTE)
// ==============================================================================
void loop() {
  // 1. Processa botões e interface
  handleKeypad();

  // 2. Controle do movimento da palheta
  if (currentState == STATE_RUN) {
    unsigned long nowMicros = micros();
    unsigned long stepInterval = SPEED_DELAY_US[speedLevel];

    if (nowMicros - lastStepMicros >= stepInterval) {
      lastStepMicros = nowMicros;

      // Executa um meio-passo na direção atual
      stepMotor(motionDirection);

      // Inversão de direção nos limites de amplitude (Palhetada alternada)
      if (currentStepPos >= amplitudeSteps) {
        motionDirection = -1;
      } else if (currentStepPos <= -amplitudeSteps) {
        motionDirection = 1;
      }
    }
  }

  // 3. Atualiza periodicamente o display LCD
  updateDisplay(false);
}
