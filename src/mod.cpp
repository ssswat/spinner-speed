// ============================================================================
//  Spinner Speed - mod para o Dusklight (Twilight Princess PC)
//
//  Cada vez que Link, em cima do Spinner, ATIVA AS SERRAS (a propria checagem
//  do jogo "ataque do Spinner"), o Spinner ACELERA progressivamente ate um
//  limite maximo, durante um curto periodo. Apertar de novo renova o boost.
//  Nao depende de bota nem de segurar botao.
//
//  >>> Para ajustar o "feeling", mexa SO nos numeros da secao CONFIG. <<<
//
//  O mod escreve no log (janela Mods > Spinner Speed > Logs).
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

// TETO de velocidade (unidades por tick). O Spinner normal anda ~26.
// (O cheat "Fast Spinner" do Dusklight limita em 60.)
constexpr float kSpeedCap = 90.0f;

// Quanto a velocidade AUMENTA por tick enquanto o boost esta ativo.
// O jogo roda a 30 ticks por segundo: 1.6 => sai de 26 e chega ao teto em ~1,3 s.
constexpr float kAccelPerTick = 1.6f;

// Duracao do boost a cada ativacao das serras, em ticks (30 = 1 segundo).
// Apertar de novo durante o boost renova esse tempo.
constexpr int kBoostTicks = 36;

// Abaixo desta velocidade o Spinner ja esta "morrendo": nao damos boost
// (evita ressuscitar um giro que acabou).
constexpr float kMinSpeedToBoost = 2.0f;

// ----------------------------------------------------------------------------
//  Estado interno (nao precisa mexer)
// ----------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

int g_boostTicks = 0;                 // ticks restantes de boost
bool g_prevAttack = false;            // valor anterior do "ataque do Spinner"
bool g_firstSample = true;            // primeira leitura deste giro?
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
        g_boostTicks = 0;
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
    } else {
        // Passou de desligado para ligado => serras ativadas (de novo).
        if (attack && !g_prevAttack) {
            g_boostTicks = kBoostTicks;
            svc_log->info(mod_ctx, "Serras ativadas! Boost iniciado/renovado");
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

    if (g_boostTicks <= 0) return HOOK_CONTINUE;
    --g_boostTicks;

    const float before = spinner->speedF;

    // So acelera se o Spinner ainda esta andando e abaixo do teto.
    // (Se ja esta acima do teto, ex.: trilhos rapidos, nao mexemos.)
    if (before >= kMinSpeedToBoost && before < kSpeedCap) {
        float after = before + kAccelPerTick;
        if (after > kSpeedCap) after = kSpeedCap;
        spinner->speedF = after;

        if ((g_logTick++ % 10) == 0) {
            logf("Boost: velocidade %.1f -> %.1f (teto %.0f)", before, after, kSpeedCap);
        }
    }
    return HOOK_CONTINUE;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    g_boostTicks = 0;
    g_prevAttack = false;
    g_firstSample = true;
    g_lastSpinner = nullptr;
    g_loggedFirstWait = false;
    g_loggedFirstMove = false;

    mods::hook::add_pre<LinkSpinnerWait>(on_link_spinner_wait_pre);
    mods::hook::add_pre<SpinnerPosMove>(on_spinner_move_pre);

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
