// ============================================================================
//  Spinner Speed - mod para o Dusklight (Twilight Princess PC)
//
//  1) BOOST: cada vez que Link, em cima do Spinner, ATIVA AS SERRAS, o Spinner
//     ganha velocidade. Atacar varias vezes seguidas soma, ate o maximo.
//  2) CURVA: o Spinner vira mais rapido (reforca a curva do proprio jogo).
//     A frenagem continua como no jogo original.
//  3) Link ENCARA a direcao em que o Spinner esta indo (giro suave).
//  4) MODO MOTOR (opcional): segurando A, o Spinner nao para de acelerar.
//  Nos TRILHOS o mod nao faz nada: o jogo controla o Spinner normalmente.
//
//  Opcoes ajustaveis DENTRO DO JOGO (janela Mods > Spinner Speed):
//    - Velocidade maxima
//    - Ganho de velocidade por ataque de serra
//    - Curva (100 = normal do jogo)
//    - Link encara o Spinner (metodo 0 a 4)
//    - Modo motor (liga/desliga) e aceleracao do motor
//
//  O mod escreve eventos no log (janela Mods > Spinner Speed > Logs).
// ============================================================================

// IMPORTANTE: a ORDEM dos includes importa (principalmente no Windows/MSVC):
//   1) bibliotecas padrao do C++ primeiro;
//   2) headers do SDK de mods;
//   3) por ultimo, os headers do jogo.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "mods/service.hpp"
#include "mods/svc/log.h"
#include "mods/svc/hook.hpp"
#include "mods/svc/config.h"
#include "mods/svc/ui.h"

#if __has_include("d/dolzel_rel.h")
#include "d/dolzel_rel.h"
#elif __has_include("d/dolzel.h")
#include "d/dolzel.h"
#endif
#include "d/d_com_inf_game.h"
#include "d/actor/d_a_spinner.h"   // classe daSpinner_c
#include "d/actor/d_a_alink.h"     // classe daAlink_c (Link)
#include "m_Do/m_Do_controller_pad.h"  // botoes do jogo (A segurado)

DEFINE_MOD();                               // uma vez so, neste arquivo
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);
IMPORT_SERVICE(ConfigService, svc_config);
IMPORT_SERVICE(UiService, svc_ui);

// Funcoes do jogo que vamos "espiar":
//  - daSpinner_c::posMove      : movimento do Spinner (1x por tick)
//  - daAlink_c::procSpinnerWait: Link andando no Spinner (1x por tick)
DEFINE_HOOK(&daSpinner_c::posMove, SpinnerPosMove);
DEFINE_HOOK(&daAlink_c::procSpinnerWait, LinkSpinnerWait);

// ----------------------------------------------------------------------------
//  CONFIG  (valores iniciais; dentro do jogo voce muda os dois primeiros)
// ----------------------------------------------------------------------------
namespace {

// Velocidade maxima padrao (unidades por tick). O Spinner normal anda ~26.
constexpr int kDefaultMaxSpeed = 90;

// Quanto cada ataque de serra soma de velocidade (padrao).
constexpr int kDefaultStepPerAttack = 8;

// Depois de um ataque, o jogo "segura" a velocidade nesse tempo (em ticks;
// 30 ticks = 1 segundo). Atacar de novo dentro desse tempo soma mais velocidade.
constexpr int kHoldTicks = 36;

// Quao rapido a velocidade sobe ate o novo valor (unidades por tick).
// Valor menor = subida mais suave.
constexpr float kRampPerTick = 1.0f;

// Link encara a direcao em que o Spinner anda: quanto da diferenca de angulo ele
// corrige por tick (30 ticks = 1 segundo). 20 = suave; 50 = rapido; 100 = instantaneo.
constexpr int kFacePercentPerTick = 20;

// Limite de giro do Link por tick, em unidades do jogo (65536 = volta inteira).
// 2500 = ~14 graus por tick, so para evitar um giro brusco em mudancas muito grandes.
constexpr int kFaceMaxStep = 2500;

// Abaixo desta velocidade o Spinner ja esta "morrendo": nao damos boost.
constexpr float kMinSpeedToBoost = 2.0f;

// ----------------------------------------------------------------------------
//  Estado interno (nao precisa mexer)
// ----------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

ConfigVarHandle g_varMaxSpeed = 0;
ConfigVarHandle g_varStep = 0;
ConfigVarHandle g_varTurn = 0;
ConfigVarHandle g_varFace = 0;
ConfigVarHandle g_varMotor = 0;
ConfigVarHandle g_varMotorAccel = 0;

float g_target = 0.0f;                // velocidade-alvo atual (soma dos ataques)
int g_holdTicks = 0;                  // ticks restantes segurando o alvo
bool g_prevAttack = false;            // valor anterior do "ataque do Spinner"
bool g_firstSample = true;            // primeira leitura deste giro?
const daSpinner_c* g_lastSpinner = nullptr;
Clock::time_point g_lastWait{};       // ultima vez que Link esteve "no Spinner"
bool g_loggedFirstWait = false;
bool g_loggedFirstMove = false;

void log_msg(const char* fmt, double a = 0, double b = 0, double c = 0) {
    char buf[200];
    std::snprintf(buf, sizeof(buf), fmt, a, b, c);
    svc_log->info(mod_ctx, buf);
}

void log_fmt(const char* fmt, ...) {
    char buf[300];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    svc_log->info(mod_ctx, buf);
}

int read_cfg_int(ConfigVarHandle var, int fallback, int lo, int hi) {
    int64_t v = fallback;
    if (var != 0) {
        if (svc_config->get_int(mod_ctx, var, &v) != MOD_OK) v = fallback;
    }
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return static_cast<int>(v);
}

float max_speed() { return static_cast<float>(read_cfg_int(g_varMaxSpeed, kDefaultMaxSpeed, 30, 300)); }
// Curva: 100 = igual ao jogo, 300 = 3x mais rapida.
int turn_percent() { return read_cfg_int(g_varTurn, 300, 100, 800); }
// Link encara o Spinner: 0 = desligado, 1 = so o modelo, 2 = modelo + direcao,
// 3 = funcao do proprio jogo (setPlayerPosAndAngle),
// 4 = EXPERIMENTAL: escreve tambem nos campos de Link que guardam a direcao inicial.
int face_method() { return read_cfg_int(g_varFace, 3, 0, 4); }
// Modo motor: 0 = desligado, 1 = ligado.
int motor_on() { return read_cfg_int(g_varMotor, 0, 0, 1); }
// Aceleracao do motor em unidades por SEGUNDO (30 ticks = 1 s).
float motor_accel_per_tick() {
    return static_cast<float>(read_cfg_int(g_varMotorAccel, 45, 5, 300)) / 30.0f;
}
float step_per_attack() { return static_cast<float>(read_cfg_int(g_varStep, kDefaultStepPerAttack, 1, 60)); }

int g_lastD = 0;                  // ultima mudanca de angulo feita pelo jogo (1 tick)
int g_noAssistTicks = 0;          // pausa do reforco de curva (anti-oscilacao)
bool g_havePost = false;
int g_preCur = 0;    // angulo do Spinner no inicio do tick
int g_postCur = 0;   // angulo do Spinner no fim do tick anterior
float g_postSpeed = 0.0f;  // velocidade do Spinner no fim do tick anterior
daAlink_c* g_link = nullptr;        // Link (guardado no hook procSpinnerWait)
int g_faceWritten = 0;              // ultimo angulo que escrevemos em Link
bool g_faceHave = false;
int g_faceLog = 0;
bool g_lastHeld = false;

// Le o botao A do JOGO (ja com o mapeamento que voce fez no Dusklight).
// Se a funcao nao existir nesta versao, devolve falso (e usamos o aviso do proprio Spinner).
template <class Pad>
bool read_hold_a() {
    if constexpr (requires { Pad::getHoldA(0); }) {
        return Pad::getHoldA(0) != 0;
    } else {
        return false;
    }
}

// ---------------------------------------------------------------------------
//  Busca de campos: o jogo refaz o shape_angle do Link a cada tick a partir de
//  OUTRA variavel (a direcao em que ele subiu). Varremos o objeto do Link atras
//  de outros valores de 16 bits iguais ao angulo inicial.
// ---------------------------------------------------------------------------
constexpr int kMaxCand = 24;
std::ptrdiff_t g_candOff[kMaxCand];
int g_candCount = 0;
bool g_scanDone = false;
int16_t g_faceSmooth = 0;        // angulo "suavizado" que queremos mostrar (metodo 4)
bool g_smoothInit = false;
int g_candLog = 0;

void scan_link_angles(daAlink_c* link) {
    g_candCount = 0;
    const unsigned char* base = reinterpret_cast<const unsigned char*>(link);
    const int16_t target = link->shape_angle.y;
    const std::ptrdiff_t offShape = reinterpret_cast<const unsigned char*>(&link->shape_angle.y) - base;
    const std::ptrdiff_t offCur = reinterpret_cast<const unsigned char*>(&link->current.angle.y) - base;
    const std::size_t startAt = sizeof(fopAc_ac_c);   // ignora os campos comuns de qualquer ator
    for (std::size_t off = startAt; off + 2 <= sizeof(daAlink_c); off += 2) {
        int16_t v;
        std::memcpy(&v, base + off, 2);
        if (v == target && static_cast<std::ptrdiff_t>(off) != offShape &&
            static_cast<std::ptrdiff_t>(off) != offCur && g_candCount < kMaxCand) {
            g_candOff[g_candCount++] = static_cast<std::ptrdiff_t>(off);
        }
    }
    char buf[320];
    int n = std::snprintf(buf, sizeof(buf), "FACE-SCAN: valor %d | %d campo(s) iguais no Link (fora shape/current):",
                          static_cast<int>(target), g_candCount);
    for (int i = 0; i < g_candCount && n > 0 && n < static_cast<int>(sizeof(buf)) - 12; ++i) {
        n += std::snprintf(buf + n, sizeof(buf) - n, " +0x%X", static_cast<unsigned>(g_candOff[i]));
    }
    svc_log->info(mod_ctx, buf);
}

void log_candidate_values(daAlink_c* link) {
    if (g_candCount == 0) return;
    const unsigned char* base = reinterpret_cast<const unsigned char*>(link);
    char buf[320];
    int n = std::snprintf(buf, sizeof(buf), "FACE-CAND:");
    for (int i = 0; i < g_candCount && n > 0 && n < static_cast<int>(sizeof(buf)) - 20; ++i) {
        int16_t v;
        std::memcpy(&v, base + g_candOff[i], 2);
        n += std::snprintf(buf + n, sizeof(buf) - n, " +0x%X=%d", static_cast<unsigned>(g_candOff[i]), static_cast<int>(v));
    }
    svc_log->info(mod_ctx, buf);
}

// Metodo 4: escreve o angulo suavizado no shape_angle e nos campos candidatos.
void write_face_candidates(daAlink_c* link) {
    link->shape_angle.y = g_faceSmooth;
    unsigned char* base = reinterpret_cast<unsigned char*>(link);
    for (int i = 0; i < g_candCount; ++i) {
        const int16_t v = g_faceSmooth;
        std::memcpy(base + g_candOff[i], &v, 2);
    }
}

// Chama a funcao do proprio jogo que posiciona/vira o Link (a mesma usada em cenas).
// Se ela nao existir nesta versao, devolve falso e nada e chamado.
template <class L>
bool game_set_pos_angle(L* link, int16_t angle) {
    if constexpr (requires { link->setPlayerPosAndAngle(&link->current.pos, angle, 0); }) {
        link->setPlayerPosAndAngle(&link->current.pos, angle, 0);
        return true;
    } else {
        return false;
    }
}

int g_faceCalls = 0;
bool g_warnedNoGameFn = false;

// Gira a frente de Link, aos poucos, para a direcao em que o Spinner esta indo.
void apply_face(daAlink_c* link, daSpinner_c* spinner) {
    const int method = face_method();
    if (method == 0 || link == nullptr || spinner == nullptr) return;
    if (spinner->checkPathMoveNow() != nullptr) return;   // nos trilhos: nao mexe

    const int delta = static_cast<int16_t>(spinner->current.angle.y - link->shape_angle.y);

    // Diagnostico: angulos do Link e do Spinner (a cada ~20 chamadas).
    if ((g_faceCalls++ % 20) == 0) {
        log_fmt("FACE: metodo=%d | Link shape=%d cur=%d | Spinner cur=%d | diferenca=%d",
                method, static_cast<int>(link->shape_angle.y), static_cast<int>(link->current.angle.y),
                static_cast<int>(spinner->current.angle.y), delta);
    }
    if ((g_candLog++ % 40) == 0) log_candidate_values(link);

    if (method >= 4) {                       // metodo 4: o estado suavizado e atualizado no hook "pre"
        if (g_smoothInit) write_face_candidates(link);
        return;
    }

    if (delta == 0) return;

    int step = delta * kFacePercentPerTick / 100;
    if (step == 0) step = delta > 0 ? 1 : -1;               // termina o ajuste fino
    if (step > kFaceMaxStep) step = kFaceMaxStep;
    if (step < -kFaceMaxStep) step = -kFaceMaxStep;

    const int16_t shape = static_cast<int16_t>(link->shape_angle.y + step);

    if (method >= 3) {
        if (!game_set_pos_angle(link, shape)) {
            if (!g_warnedNoGameFn) {
                g_warnedNoGameFn = true;
                svc_log->warn(mod_ctx, "FACE: funcao do jogo indisponivel; usando o metodo 2");
            }
            link->shape_angle.y = shape;
            link->current.angle.y = static_cast<int16_t>(link->current.angle.y + step);
        }
    } else {
        link->shape_angle.y = shape;
        if (method >= 2) {
            link->current.angle.y = static_cast<int16_t>(link->current.angle.y + step);
        }
    }
    g_faceWritten = static_cast<int>(shape);
    g_faceHave = true;
}

// Roda ANTES de cada tick em que Link esta no Spinner.
// Aqui detectamos a ativacao das serras.
HookAction on_link_spinner_wait_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);   // arg 0 = "this"
    if (link == nullptr) return HOOK_CONTINUE;
    g_link = link;

    // Diagnostico da orientacao de Link: o jogo desfez o que escrevemos no tick anterior?
    if (g_faceHave) {
        const int now_shape = static_cast<int>(link->shape_angle.y);
        if (now_shape != g_faceWritten && (g_faceLog++ % 20) == 0) {
            log_msg("FACE: escrevi %.0f mas o jogo deixou em %.0f (sobrescrito)", g_faceWritten, now_shape);
        }
    }

    if (!g_loggedFirstWait) {
        g_loggedFirstWait = true;
        svc_log->info(mod_ctx, "Link esta no Spinner (hook procSpinnerWait funcionando)");
    }

    const auto now = Clock::now();
    const double since = std::chrono::duration<double>(now - g_lastWait).count();
    daSpinner_c* spinner = link->getSpinnerActor();

    // Giro novo (passou mais de 1s sem estar no Spinner, ou outro Spinner)?
    if (since > 1.0 || spinner != g_lastSpinner) {
        g_target = 0.0f;
        g_holdTicks = 0;
        g_lastSpinner = spinner;
        g_firstSample = true;
        g_havePost = false;   // nao compara com o giro anterior
        g_lastD = 0;
        g_noAssistTicks = 0;
        g_faceHave = false;
        g_scanDone = false;
        g_smoothInit = false;
        svc_log->info(mod_ctx, "Novo giro de Spinner: aguardando ativar as serras");
    }
    g_lastWait = now;

    // Varredura (uma vez por giro): procura onde o jogo guarda a direcao inicial do Link.
    if (!g_scanDone) {
        g_scanDone = true;
        scan_link_angles(link);
    }

    // Metodo 4: atualiza o angulo suavizado e escreve ANTES do jogo refazer o shape_angle.
    if (face_method() >= 4 && spinner != nullptr && spinner->checkPathMoveNow() == nullptr) {
        if (!g_smoothInit) {
            g_faceSmooth = link->shape_angle.y;
            g_smoothInit = true;
        }
        const int d4 = static_cast<int16_t>(spinner->current.angle.y - g_faceSmooth);
        int st4 = d4 * kFacePercentPerTick / 100;
        if (st4 == 0 && d4 != 0) st4 = d4 > 0 ? 1 : -1;
        if (st4 > kFaceMaxStep) st4 = kFaceMaxStep;
        if (st4 < -kFaceMaxStep) st4 = -kFaceMaxStep;
        g_faceSmooth = static_cast<int16_t>(g_faceSmooth + st4);
        write_face_candidates(link);
    }

    // Checagem do proprio jogo: "as serras / o ataque do Spinner foi acionado?"
    const bool attack = link->checkSpinnerTriggerAttack();

    if (g_firstSample) {
        // Primeira leitura do giro = referencia (ignora o aperto de subir no Spinner).
        g_firstSample = false;
        g_prevAttack = attack;
    } else {
        // Passou de desligado para ligado => serras ativadas (de novo).
        if (attack && !g_prevAttack && spinner != nullptr && spinner->checkPathMoveNow() == nullptr) {
            const float cap = max_speed();
            const float step = step_per_attack();
            // Parte da velocidade atual (ou do alvo anterior, se for maior) e soma um degrau.
            float base = spinner->speedF;
            if (g_holdTicks > 0 && g_target > base) base = g_target;
            float next = base + step;
            if (next > cap) next = cap;
            g_target = next;
            g_holdTicks = kHoldTicks;
            log_msg("Serras ativadas! Nova velocidade-alvo %.1f (maxima %.0f)", g_target, cap);
        }
        g_prevAttack = attack;
    }
    return HOOK_CONTINUE;
}

// Roda DEPOIS de cada tick em que Link esta no Spinner: vira a frente de Link,
// aos poucos, para a direcao em que o Spinner esta indo.
void on_link_spinner_wait_post(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);
    if (link == nullptr) return;
    apply_face(link, link->getSpinnerActor());
}

// Roda ANTES do movimento do Spinner (1x por tick).
HookAction on_spinner_move_pre(ModContext*, void* args, void*, void*) {
    daSpinner_c* spinner = mods::arg<daSpinner_c*>(args, 0);
    if (spinner == nullptr) return HOOK_CONTINUE;

    if (!g_loggedFirstMove) {
        g_loggedFirstMove = true;
        svc_log->info(mod_ctx, "Movimento do Spinner detectado (hook posMove funcionando)");
    }

    // Nos TRILHOS o mod nao mexe em nada (nem velocidade, nem direcao):
    // o jogo controla o Spinner sozinho. Zera o estado para nao agir ao sair do trilho.
    if (spinner->checkPathMoveNow() != nullptr) {
        g_holdTicks = 0;
        g_target = 0.0f;
        g_noAssistTicks = 0;
        g_lastD = 0;
        g_havePost = false;
        return HOOK_CONTINUE;
    }

    g_preCur = static_cast<int>(spinner->current.angle.y);

    // ------------------------------------------------------------------
    //  CURVA MAIS LIVRE: o jogo gira o Spinner ate 360 unidades por tick.
    //  Medimos quanto o jogo girou neste tick e somamos um reforco na
    //  mesma direcao (curva_percent/100 vezes mais rapido).
    // ------------------------------------------------------------------
    if (g_havePost) {
        const int d = static_cast<int16_t>(g_preCur - g_postCur);   // girada pelo jogo neste tick
        if (d != 0) {
            // Mudou de sentido? Provavel ultrapassagem do alvo: pausa o reforco um instante.
            if (g_lastD != 0 && ((d > 0) != (g_lastD > 0))) g_noAssistTicks = 8;
            g_lastD = d;
        }
        if (g_noAssistTicks > 0) --g_noAssistTicks;

        const int ad = d < 0 ? -d : d;
        // So reforca passos "cheios" do jogo (360); o passo final, que chega no alvo, nao.
        if (g_noAssistTicks == 0 && ad >= 300) {
            const int pct = turn_percent();
            const int extra = static_cast<int>(static_cast<float>(d) * (static_cast<float>(pct) / 100.0f - 1.0f));
            if (extra != 0) {
                spinner->current.angle.y = static_cast<int16_t>(spinner->current.angle.y + extra);
                spinner->shape_angle.y = static_cast<int16_t>(spinner->shape_angle.y + extra);
            }
        }
    }

    // ------------------------------------------------------------------
    //  MODO MOTOR (opcional): segurando A, o Spinner acelera sem parar
    //  ate a velocidade maxima, em vez de depender de cada ataque.
    // ------------------------------------------------------------------
    {
        const bool holdA = read_hold_a<mDoCPd_c>();
        const bool holdJump = spinner->getButtonJump() != 0;   // aviso do proprio Spinner
        const bool held = holdA || holdJump;
        if (held != g_lastHeld) {
            g_lastHeld = held;
            log_msg("Botao de ataque: segurando=%.0f (A do jogo=%.0f, aviso do Spinner=%.0f)",
                    held ? 1.0 : 0.0, holdA ? 1.0 : 0.0, holdJump ? 1.0 : 0.0);
        }
        if (motor_on() != 0 && held) {
            const float capM = max_speed();
            const float v = spinner->speedF;
            if (v >= kMinSpeedToBoost && v < capM) {
                // Perda natural de velocidade que o jogo aplicou desde o fim do tick anterior:
                // devolvemos, para a subida ficar constante ate o maximo.
                float loss = 0.0f;
                if (g_havePost) {
                    loss = g_postSpeed - v;
                    if (loss < 0.0f || loss > 5.0f) loss = 0.0f;
                }
                float nv = v + loss + motor_accel_per_tick();
                if (nv > capM) nv = capM;
                spinner->speedF = nv;
            }
            g_holdTicks = 0;   // o motor substitui o boost por ataque enquanto segura
            g_target = 0.0f;
            return HOOK_CONTINUE;
        }
    }

    if (g_holdTicks <= 0) {
        g_target = 0.0f;   // acabou o periodo: proximo ataque parte da velocidade real
        return HOOK_CONTINUE;
    }
    --g_holdTicks;

    const float cap = max_speed();
    if (g_target > cap) g_target = cap;

    const float before = spinner->speedF;

    // So sobe se o Spinner ainda esta andando e abaixo do alvo.
    // (Se ja esta mais rapido que o alvo, ex.: trilhos rapidos, nao mexemos.)
    if (before >= kMinSpeedToBoost && before < g_target) {
        float after = before + kRampPerTick;
        if (after > g_target) after = g_target;
        spinner->speedF = after;

    }
    return HOOK_CONTINUE;
}

// Roda DEPOIS do movimento do Spinner: guarda o angulo final deste tick,
// para medirmos no proximo tick quanto o jogo girou o Spinner.
void on_spinner_move_post(ModContext*, void* args, void*, void*) {
    daSpinner_c* spinner = mods::arg<daSpinner_c*>(args, 0);
    if (spinner == nullptr) return;
    if (spinner->checkPathMoveNow() != nullptr) {   // nos trilhos: nao mede a curva
        g_havePost = false;
        return;
    }
    g_postCur = static_cast<int>(spinner->current.angle.y);
    g_postSpeed = spinner->speedF;
    g_havePost = true;

    // Segundo ponto de escrita da orientacao de Link (fim do tick do Spinner).
    const double since = std::chrono::duration<double>(Clock::now() - g_lastWait).count();
    if (g_link != nullptr && since < 0.5) apply_face(g_link, spinner);
}

// Painel do mod na janela Mods: as duas opcoes ajustaveis.
ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "Spinner Speed");

    UiControlDesc maxCtl = UI_CONTROL_DESC_INIT;
    maxCtl.kind = UI_CONTROL_NUMBER;
    maxCtl.label = "Velocidade maxima";
    maxCtl.help_rml = "Velocidade maxima do Spinner com o boost. O normal do jogo e cerca de 26.";
    maxCtl.binding = UI_BINDING_CONFIG_VAR;
    maxCtl.config_var = g_varMaxSpeed;
    maxCtl.min = 30;
    maxCtl.max = 300;
    maxCtl.step = 5;
    svc_ui->pane_add_control(mod_ctx, panel, &maxCtl, nullptr);

    UiControlDesc stepCtl = UI_CONTROL_DESC_INIT;
    stepCtl.kind = UI_CONTROL_NUMBER;
    stepCtl.label = "Ganho por ataque de serra";
    stepCtl.help_rml = "Quanto de velocidade cada ataque de serra soma. Menor = mais ataques para chegar ao maximo.";
    stepCtl.binding = UI_BINDING_CONFIG_VAR;
    stepCtl.config_var = g_varStep;
    stepCtl.min = 1;
    stepCtl.max = 60;
    stepCtl.step = 1;
    svc_ui->pane_add_control(mod_ctx, panel, &stepCtl, nullptr);

    UiControlDesc turnCtl = UI_CONTROL_DESC_INIT;
    turnCtl.kind = UI_CONTROL_NUMBER;
    turnCtl.label = "Curva (100 = normal do jogo)";
    turnCtl.help_rml = "Quao rapido o Spinner vira. 100 e o normal do jogo; 300 vira 3 vezes mais rapido.";
    turnCtl.binding = UI_BINDING_CONFIG_VAR;
    turnCtl.config_var = g_varTurn;
    turnCtl.min = 100;
    turnCtl.max = 800;
    turnCtl.step = 25;
    svc_ui->pane_add_control(mod_ctx, panel, &turnCtl, nullptr);

    UiControlDesc faceCtl = UI_CONTROL_DESC_INIT;
    faceCtl.kind = UI_CONTROL_NUMBER;
    faceCtl.label = "Link encara o Spinner (0 = desligado, 1 a 4)";
    faceCtl.help_rml = "Faz o Link olhar para onde o Spinner vai. Se um metodo nao funcionar, tente outro: 1 = so o modelo, 2 = modelo e direcao, 3 = funcao do proprio jogo, 4 = experimental (escreve nos campos que guardam a direcao inicial).";
    faceCtl.binding = UI_BINDING_CONFIG_VAR;
    faceCtl.config_var = g_varFace;
    faceCtl.min = 0;
    faceCtl.max = 4;
    faceCtl.step = 1;
    svc_ui->pane_add_control(mod_ctx, panel, &faceCtl, nullptr);

    UiControlDesc motorCtl = UI_CONTROL_DESC_INIT;
    motorCtl.kind = UI_CONTROL_NUMBER;
    motorCtl.label = "Modo motor (0 = desligado, 1 = ligado)";
    motorCtl.help_rml = "Ligado: segurando A, o Spinner acelera sem parar ate a velocidade maxima.";
    motorCtl.binding = UI_BINDING_CONFIG_VAR;
    motorCtl.config_var = g_varMotor;
    motorCtl.min = 0;
    motorCtl.max = 1;
    motorCtl.step = 1;
    svc_ui->pane_add_control(mod_ctx, panel, &motorCtl, nullptr);

    UiControlDesc accelCtl = UI_CONTROL_DESC_INIT;
    accelCtl.kind = UI_CONTROL_NUMBER;
    accelCtl.label = "Aceleracao do motor (por segundo)";
    accelCtl.help_rml = "Quanto de velocidade o modo motor soma por segundo, sem a perda natural do Spinner. Maior = chega mais rapido ao maximo. 45 leva uns 2 segundos para ir de 26 a 100.";
    accelCtl.binding = UI_BINDING_CONFIG_VAR;
    accelCtl.config_var = g_varMotorAccel;
    accelCtl.min = 5;
    accelCtl.max = 300;
    accelCtl.step = 5;
    svc_ui->pane_add_control(mod_ctx, panel, &accelCtl, nullptr);

    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    g_target = 0.0f;
    g_holdTicks = 0;
    g_prevAttack = false;
    g_firstSample = true;
    g_lastSpinner = nullptr;
    g_loggedFirstWait = false;
    g_loggedFirstMove = false;
    g_havePost = false;
    g_lastD = 0;
    g_noAssistTicks = 0;
    g_link = nullptr;
    g_faceHave = false;
    g_faceCalls = 0;
    g_scanDone = false;
    g_smoothInit = false;
    g_candCount = 0;
    g_candLog = 0;
    g_warnedNoGameFn = false;
    g_lastHeld = false;

    // Opcoes salvas no config do Dusklight.
    ConfigVarDesc maxDesc = CONFIG_VAR_DESC_INIT;
    maxDesc.name = "maxSpeed";
    maxDesc.type = CONFIG_VAR_INT;
    maxDesc.default_int = kDefaultMaxSpeed;
    if (svc_config->register_var(mod_ctx, &maxDesc, &g_varMaxSpeed) != MOD_OK) {
        g_varMaxSpeed = 0;
        svc_log->warn(mod_ctx, "Nao foi possivel registrar a opcao de velocidade maxima (usando padrao)");
    }

    ConfigVarDesc turnDesc = CONFIG_VAR_DESC_INIT;
    turnDesc.name = "turnPercent";
    turnDesc.type = CONFIG_VAR_INT;
    turnDesc.default_int = 300;
    if (svc_config->register_var(mod_ctx, &turnDesc, &g_varTurn) != MOD_OK) {
        g_varTurn = 0;
        svc_log->warn(mod_ctx, "Nao foi possivel registrar a opcao de curva (usando padrao)");
    }

    ConfigVarDesc faceDesc = CONFIG_VAR_DESC_INIT;
    faceDesc.name = "faceMethod";
    faceDesc.type = CONFIG_VAR_INT;
    faceDesc.default_int = 3;
    if (svc_config->register_var(mod_ctx, &faceDesc, &g_varFace) != MOD_OK) {
        g_varFace = 0;
        svc_log->warn(mod_ctx, "Nao foi possivel registrar a opcao de orientacao do Link (usando padrao)");
    }

    ConfigVarDesc motorDesc = CONFIG_VAR_DESC_INIT;
    motorDesc.name = "motorMode";
    motorDesc.type = CONFIG_VAR_INT;
    motorDesc.default_int = 0;
    if (svc_config->register_var(mod_ctx, &motorDesc, &g_varMotor) != MOD_OK) {
        g_varMotor = 0;
        svc_log->warn(mod_ctx, "Nao foi possivel registrar a opcao do modo motor (usando padrao)");
    }

    ConfigVarDesc accelDesc = CONFIG_VAR_DESC_INIT;
    accelDesc.name = "motorAccel";
    accelDesc.type = CONFIG_VAR_INT;
    accelDesc.default_int = 45;
    if (svc_config->register_var(mod_ctx, &accelDesc, &g_varMotorAccel) != MOD_OK) {
        g_varMotorAccel = 0;
        svc_log->warn(mod_ctx, "Nao foi possivel registrar a aceleracao do motor (usando padrao)");
    }

    ConfigVarDesc stepDesc = CONFIG_VAR_DESC_INIT;
    stepDesc.name = "stepPerAttack";
    stepDesc.type = CONFIG_VAR_INT;
    stepDesc.default_int = kDefaultStepPerAttack;
    if (svc_config->register_var(mod_ctx, &stepDesc, &g_varStep) != MOD_OK) {
        g_varStep = 0;
        svc_log->warn(mod_ctx, "Nao foi possivel registrar a opcao de ganho por ataque (usando padrao)");
    }

    // Painel com as opcoes na janela Mods (so se as opcoes existem).
    if (g_varMaxSpeed != 0 && g_varStep != 0 && g_varTurn != 0 && g_varFace != 0 &&
        g_varMotor != 0 && g_varMotorAccel != 0) {
        UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
        panel.build = build_panel;
        if (svc_ui->register_mods_panel(mod_ctx, &panel) != MOD_OK) {
            svc_log->warn(mod_ctx, "Nao foi possivel criar o painel de opcoes");
        }
    }

    mods::hook::add_pre<LinkSpinnerWait>(on_link_spinner_wait_pre);
    mods::hook::add_post<LinkSpinnerWait>(on_link_spinner_wait_post);
    mods::hook::add_pre<SpinnerPosMove>(on_spinner_move_pre);
    mods::hook::add_post<SpinnerPosMove>(on_spinner_move_post);

    svc_log->info(mod_ctx, "Spinner Speed carregado!");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError* error) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError* error) {
    return MOD_OK;
}

}
