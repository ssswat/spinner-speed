// ============================================================================
//  Spinner Speed - mod para o Dusklight (Twilight Princess PC)
//
//  Quando Link, JA EM CIMA do Spinner, ATIVA AS SERRAS (a propria checagem do
//  jogo "ataque do Spinner" - nao importa qual botao seja), a velocidade do
//  Spinner passa a aumentar PROGRESSIVAMENTE ate um limite maximo.
//  Nao precisa segurar nenhum botao, e nao depende de bota.
//  O efeito dura ate o fim daquele giro do Spinner.
//
//  >>> Para ajustar o "feeling", mexa SO nos numeros da secao CONFIG. <<<
//
//  O mod escreve mensagens no log (janela Mods > Spinner Speed > Logs) para
//  ajudar a descobrir se algo nao funcionar.
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

// Funcoes do jogo que vamos "espiar":
//  - daSpinner_c::posMove      : movimento do Spinner (1x por tick)
//  - daAlink_c::procSpinnerWait: Link andando no Spinner (1x por tick)
DEFINE_HOOK(&daSpinner_c::posMove, SpinnerPosMove);
DEFINE_HOOK(&daAlink_c::procSpinnerWait, LinkSpinnerWait);

// ----------------------------------------------------------------------------
//  CONFIG  (pode editar a vontade)
// ----------------------------------------------------------------------------
namespace {

// Multiplicador maximo de velocidade. 1.0 = normal, 2.0 = o dobro.
constexpr float kMaxMultiplier = 2.0f;

// Teto ABSOLUTO de velocidade (unidades por tick). O Spinner normal anda ~26.
// (O cheat "Fast Spinner" do Dusklight limita em 60.)
constexpr float kSpeedCap = 48.0f;

// Quanto o multiplicador sobe por tick depois de ativar (30 ticks = 1 segundo).
// 0.04 => ~25 ticks (menos de 1 s) para ir de 1.0 ate 2.0.
constexpr float kRampPerTick = 0.04f;

// ----------------------------------------------------------------------------
//  Estado interno (nao precisa mexer)
// ----------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

bool g_active = false;                // serras foram ativadas neste giro?
bool g_prevAttack = false;            // valor anterior do "ataque do Spinner"
bool g_firstSample = true;            // primeira leitura deste giro?
float g_boost = 1.0f;                 // multiplicador atual (1.0 .. kMaxMultiplier)
float g_appliedFactor = 1.0f;         // fator aplicado neste tick (desfeito no post-hook)
const daSpinner_c* g_lastSpinner = nullptr;
Clock::time_point g_lastWait{};       // ultima vez que Link esteve "no Spinner"
bool g_loggedFirstWait = false;
bool g_loggedFirstMove = false;
int g_logTick = 0;

void logf(const char* fmt, double a = 0, double b = 0, double c = 0) {
    char buf[200];
    std::snprintf(buf, sizeof(buf), fmt, a, b, c);
    svc_log->info(mod_ctx, buf);
}

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Roda ANTES de cada tick em que Link esta no Spinner.
// Aqui detectamos a ativacao das serras.
HookAction on_link_spinner_wait_pre(ModContext*, void* args, void*, void*) {
    daAlink_c* link = mods::arg<daAlink_c*>(args, 0);   // arg 0 = "this"
    if (link == nullptr) return HOOK_CONTINUE;

    if (!g_loggedFirstWait) {
        g_loggedFirstWait = true;
        svc_log->info(mod_ctx, "Link esta no Spinner (hook procSpinnerWait funcionando)");
    }

    const auto now = Clock::now();
    const double since = std::chrono::duration<double>(now - g_lastWait).count();
    daSpinner_c* spinner = link->getSpinnerActor();

    // Giro novo (passou mais de 1s sem estar no Spinner, ou outro Spinner)?
    if (since > 1.0 || spinner != g_lastSpinner) {
        g_active = false;
        g_boost = 1.0f;
        g_lastSpinner = spinner;
        g_logTick = 0;
        g_firstSample = true;
        svc_log->info(mod_ctx, "Novo giro de Spinner: aguardando ativar as serras");
    }
    g_lastWait = now;

    // Checagem do proprio jogo: "as serras / o ataque do Spinner foi acionado?"
    const bool attack = link->checkSpinnerTriggerAttack();

    if (g_firstSample) {
        // Primeira leitura do giro = referencia (ignora o aperto de subir no Spinner).
        g_firstSample = false;
        g_prevAttack = attack;
    } else if (attack != g_prevAttack) {
        logf("Checagem de ataque do Spinner mudou para %.0f", attack ? 1.0 : 0.0);
        g_prevAttack = attack;
        // Passou de desligado para ligado => serras ativadas.
        if (attack && !g_active) {
            g_active = true;
            svc_log->info(mod_ctx, "Serras ativadas! Boost progressivo iniciado");
        }
    }
    return HOOK_CONTINUE;
}

// Roda ANTES do movimento do Spinner (1x por tick).
HookAction on_spinner_move_pre(ModContext*, void* args, void*, void*) {
    daSpinner_c* spinner = mods::arg<daSpinner_c*>(args, 0);
    g_appliedFactor = 1.0f;
    if (spinner == nullptr) return HOOK_CONTINUE;

    if (!g_loggedFirstMove) {
        g_loggedFirstMove = true;
        svc_log->info(mod_ctx, "Movimento do Spinner detectado (hook posMove funcionando)");
    }

    if (!g_active) return HOOK_CONTINUE;

    g_boost = clampf(g_boost + kRampPerTick, 1.0f, kMaxMultiplier);

    const float base = spinner->speedF;
    float factor = 1.0f;
    if (base > 0.0f && base < kSpeedCap) {
        float wanted = base * g_boost;
        if (wanted > kSpeedCap) wanted = kSpeedCap;
        factor = wanted / base;
    }
    // (Se o Spinner ja esta acima do teto, ex.: trilhos rapidos, nao mexemos.)

    g_appliedFactor = factor;
    spinner->speedF = base * factor;

    if ((g_logTick++ % 15) == 0) {
        logf("Boost: multiplicador %.2f, velocidade %.1f -> %.1f",
             g_boost, base, base * factor);
    }
    return HOOK_CONTINUE;
}

// Roda DEPOIS do movimento: desfaz o fator para a desaceleracao natural
// do Spinner continuar funcionando.
void on_spinner_move_post(ModContext*, void* args, void*, void*) {
    daSpinner_c* spinner = mods::arg<daSpinner_c*>(args, 0);
    if (spinner != nullptr && g_appliedFactor > 0.0f && g_appliedFactor != 1.0f) {
        spinner->speedF /= g_appliedFactor;
    }
    g_appliedFactor = 1.0f;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    g_active = false;
    g_prevAttack = false;
    g_firstSample = true;
    g_boost = 1.0f;
    g_appliedFactor = 1.0f;
    g_lastSpinner = nullptr;
    g_loggedFirstWait = false;
    g_loggedFirstMove = false;

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
