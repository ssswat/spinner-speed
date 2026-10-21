// ============================================================================
//  Spinner Boost - mod para o Dusklight (Twilight Princess PC)
//
//  Enquanto Link estiver no Spinner e segurando R (giro das serras), a
//  velocidade do Spinner aumenta PROGRESSIVAMENTE ate um limite maximo.
//  Soltou R? A aceleracao extra vai sumindo aos poucos.
//
//  Nao depende de bota nenhuma: so do Spinner + R.
//
//  >>> Se quiser ajustar o "feeling", mexa SO nos numeros da secao CONFIG. <<<
// ============================================================================

#include "mods/service.hpp"
#include "mods/svc/log.h"
#include "mods/svc/hook.hpp"

#include "d/actor/d_a_spinner.h"          // classe daSpinner_c
#include "m_Do/m_Do_controller_pad.h"     // leitura do controle (botao R)

DEFINE_MOD();                               // uma vez so, neste arquivo
IMPORT_SERVICE(LogService, svc_log);
IMPORT_SERVICE(HookService, svc_hook);

// Diz ao Dusklight qual funcao do jogo vamos "espiar":
// daSpinner_c::posMove = o passo de movimento do Spinner (roda 1x por tick).
DEFINE_HOOK(&daSpinner_c::posMove, SpinnerPosMove);

// ----------------------------------------------------------------------------
//  CONFIG  (pode editar a vontade)
// ----------------------------------------------------------------------------
namespace {

// Multiplicador maximo de velocidade. 1.0 = vanilla, 2.0 = o dobro.
constexpr float kMaxMultiplier = 2.0f;

// Teto ABSOLUTO de velocidade (unidades por tick). O Spinner vanilla anda
// ~26 na maioria dos lugares. Com 48 voce fica ~85% mais rapido que o normal,
// mas nao vira um foguete. (O cheat "Fast Spinner" do proprio Dusklight limita em 60.)
constexpr float kSpeedCap = 48.0f;

// Quanto o multiplicador sobe por tick enquanto R esta apertado.
// O jogo roda a 30 ticks/s: 0.02 => ~50 ticks (~1,7 s) para sair de 1.0 a 2.0.
constexpr float kRampPerTick = 0.02f;

// Quanto o multiplicador cai por tick quando solta R (mais rapido que subir).
constexpr float kDecayPerTick = 0.05f;

// ----------------------------------------------------------------------------
//  Estado interno (nao precisa mexer)
// ----------------------------------------------------------------------------
float g_boost = 1.0f;                 // multiplicador atual (1.0 .. kMaxMultiplier)
float g_appliedFactor = 1.0f;         // fator aplicado neste tick (para desfazer no post-hook)
const daSpinner_c* g_lastSpinner = nullptr;

float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// Roda ANTES do movimento do Spinner.
HookAction on_spinner_move_pre(ModContext*, void* args, void*, void*) {
    daSpinner_c* spinner = mods::arg<daSpinner_c*>(args, 0);   // arg 0 = "this"
    if (spinner == nullptr) {
        g_appliedFactor = 1.0f;
        return HOOK_CONTINUE;
    }

    // Spinner novo (acabou de subir nele)? Comeca do zero.
    if (spinner != g_lastSpinner) {
        g_lastSpinner = spinner;
        g_boost = 1.0f;
    }

    // R segurado = serras girando.
    const bool sawsSpinning = mDoCPd_c::getHoldR(PAD_1) != 0;

    if (sawsSpinning) {
        g_boost = clampf(g_boost + kRampPerTick, 1.0f, kMaxMultiplier);
    } else {
        g_boost = clampf(g_boost - kDecayPerTick, 1.0f, kMaxMultiplier);
    }

    // Calcula o fator final respeitando o teto absoluto.
    const float base = spinner->speedF;
    float factor = 1.0f;
    if (base > 0.0f && base < kSpeedCap) {
        float wanted = base * g_boost;
        if (wanted > kSpeedCap) wanted = kSpeedCap;
        factor = wanted / base;
    }
    // (Se o Spinner ja esta mais rapido que o teto, ex.: trilhos rapidos, nao mexemos.)

    g_appliedFactor = factor;
    spinner->speedF = base * factor;
    return HOOK_CONTINUE;
}

// Roda DEPOIS do movimento: desfaz o fator, para a desaceleracao natural
// do Spinner continuar funcionando normalmente.
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
    g_boost = 1.0f;
    g_appliedFactor = 1.0f;
    g_lastSpinner = nullptr;

    mods::hook::add_pre<SpinnerPosMove>(on_spinner_move_pre);
    mods::hook::add_post<SpinnerPosMove>(on_spinner_move_post);

    svc_log->info(mod_ctx, "Spinner Boost carregado!");
    return MOD_OK;
}

MOD_EXPORT ModResult mod_update(ModError* error) {
    return MOD_OK;
}

MOD_EXPORT ModResult mod_shutdown(ModError* error) {
    // O Dusklight remove os hooks sozinho ao desativar o mod.
    return MOD_OK;
}

}
