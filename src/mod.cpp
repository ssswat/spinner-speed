// ============================================================================
//  Spinner Speed - mod para o Dusklight (Twilight Princess PC)
//
//  1) BOOST: cada vez que Link, em cima do Spinner, ATIVA AS SERRAS, o Spinner
//     ganha velocidade. Atacar varias vezes seguidas soma, ate o maximo.
//  2) CURVA: o Spinner vira mais rapido (reforca a curva do proprio jogo).
//     A frenagem continua como no jogo original.
//  Nos TRILHOS o mod nao faz nada: o jogo controla o Spinner normalmente.
//
//  Opcoes ajustaveis DENTRO DO JOGO (janela Mods > Spinner Speed):
//    - Velocidade maxima
//    - Ganho de velocidade por ataque de serra
//    - Curva (100 = normal do jogo)
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

// Abaixo desta velocidade o Spinner ja esta "morrendo": nao damos boost.
constexpr float kMinSpeedToBoost = 2.0f;

// ----------------------------------------------------------------------------
//  Estado interno (nao precisa mexer)
// ----------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

ConfigVarHandle g_varMaxSpeed = 0;
ConfigVarHandle g_varStep = 0;
ConfigVarHandle g_varTurn = 0;

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
float step_per_attack() { return static_cast<float>(read_cfg_int(g_varStep, kDefaultStepPerAttack, 1, 60)); }

daAlink_c* g_link = nullptr;      // Link (guardado no hook procSpinnerWait)
int g_lastD = 0;                  // ultima mudanca de angulo feita pelo jogo (1 tick)
int g_noAssistTicks = 0;          // pausa do reforco de curva (anti-oscilacao)
bool g_havePost = false;
int g_preCur = 0;    // angulo do Spinner no inicio do tick
int g_postCur = 0;   // angulo do Spinner no fim do tick anterior

// Roda ANTES de cada tick em que Link esta no Spinner.
// Aqui detectamos a ativacao das serras.
HookAction on_link_spinner_wait_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);   // arg 0 = "this"
    if (link == nullptr) return HOOK_CONTINUE;
    g_link = link;

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
        svc_log->info(mod_ctx, "Novo giro de Spinner: aguardando ativar as serras");
    }
    g_lastWait = now;

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
                if (g_link != nullptr) {
                    g_link->current.angle.y = static_cast<int16_t>(g_link->current.angle.y + extra);
                    g_link->shape_angle.y = static_cast<int16_t>(g_link->shape_angle.y + extra);
                }
            }
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
    g_havePost = true;
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
    g_link = nullptr;
    g_lastD = 0;
    g_noAssistTicks = 0;

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

    ConfigVarDesc stepDesc = CONFIG_VAR_DESC_INIT;
    stepDesc.name = "stepPerAttack";
    stepDesc.type = CONFIG_VAR_INT;
    stepDesc.default_int = kDefaultStepPerAttack;
    if (svc_config->register_var(mod_ctx, &stepDesc, &g_varStep) != MOD_OK) {
        g_varStep = 0;
        svc_log->warn(mod_ctx, "Nao foi possivel registrar a opcao de ganho por ataque (usando padrao)");
    }

    // Painel com as opcoes na janela Mods (so se as opcoes existem).
    if (g_varMaxSpeed != 0 && g_varStep != 0 && g_varTurn != 0) {
        UiModsPanelDesc panel = UI_MODS_PANEL_DESC_INIT;
        panel.build = build_panel;
        if (svc_ui->register_mods_panel(mod_ctx, &panel) != MOD_OK) {
            svc_log->warn(mod_ctx, "Nao foi possivel criar o painel de opcoes");
        }
    }

    mods::hook::add_pre<LinkSpinnerWait>(on_link_spinner_wait_pre);
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
