// ============================================================================
//  Spinner Speed - mod para o Dusklight (Twilight Princess PC)
//
//  O que o mod faz:
//   1) MOTOR: segurando A em cima do Spinner, ele acelera sem parar, aos poucos,
//      ate a velocidade maxima. A animacao das serras fica em loop (quadros 4-5).
//   2) CURVA: o Spinner vira 1,5x mais rapido (reforca a curva do proprio jogo).
//   3) Link ENCARA a direcao em que o Spinner esta indo (giro suave).
//   A frenagem continua como no jogo original.
//   Nos TRILHOS o mod nao faz nada: o jogo controla o Spinner normalmente.
//
//  Unica opcao ajustavel dentro do jogo (janela Mods > Spinner Speed):
//    - Velocidade maxima (padrao 70)
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
#include "d/actor/d_a_spinner.h"       // classe daSpinner_c
#include "d/actor/d_a_alink.h"         // classe daAlink_c (Link)
#include "m_Do/m_Do_controller_pad.h"  // botoes do jogo (A segurado)

// Acesso ao campo PRIVADO daSpinner_c::mBck (a animacao do Spinner). A instanciacao
// explicita de um template nao verifica "private", entao conseguimos o ponteiro do campo.
template <typename Tag, typename Tag::type M>
struct FieldRobber {
    friend typename Tag::type steal_field(Tag) { return M; }
};
struct SpinnerBckTag {
    typedef mDoExt_bckAnm daSpinner_c::*type;
    friend type steal_field(SpinnerBckTag);
};
template struct FieldRobber<SpinnerBckTag, &daSpinner_c::mBck>;

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

namespace {

// ----------------------------------------------------------------------------
//  VALORES FIXOS DO MOD
// ----------------------------------------------------------------------------

// Velocidade maxima padrao (unidades por tick). O Spinner normal anda ~26.
// E a unica opcao que voce muda dentro do jogo.
constexpr int kDefaultMaxSpeed = 70;

// Ganho de velocidade do motor, em unidades por SEGUNDO (o jogo roda a 30 ticks por segundo).
// 7 por segundo = uns 6 segundos para ir de 26 ate 70.
constexpr float kMotorAccelPerSecond = 7.0f;

// Curva: 100 = igual ao jogo, 150 = 1,5x mais rapida.
constexpr int kTurnPercent = 150;

// Loop da animacao das serras (a animacao de ataque tem 15 quadros):
// ao chegar no quadro final, volta ao inicial. 4 e 5 mantem as laminas abertas.
constexpr int kLoopStartFrame = 4;
constexpr int kLoopEndFrame = 5;

// Link encara a direcao em que o Spinner anda: quanto da diferenca de angulo ele
// corrige por tick. 20 = suave; 50 = rapido; 100 = instantaneo.
constexpr int kFacePercentPerTick = 20;

// Limite de giro do Link por tick, em unidades do jogo (65536 = volta inteira).
// 2500 = ~14 graus por tick, so para evitar um giro brusco em mudancas muito grandes.
constexpr int kFaceMaxStep = 2500;

// Abaixo desta velocidade o Spinner ja esta "morrendo": o motor nao age.
constexpr float kMinSpeedToBoost = 2.0f;

// ----------------------------------------------------------------------------
//  Estado interno (nao precisa mexer)
// ----------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

ConfigVarHandle g_varMaxSpeed = 0;

const daSpinner_c* g_lastSpinner = nullptr;
Clock::time_point g_lastWait{};       // ultima vez que Link esteve "no Spinner"
bool g_loggedFirstWait = false;
bool g_loggedFirstMove = false;

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

int g_lastD = 0;                  // ultima mudanca de angulo feita pelo jogo (1 tick)
int g_noAssistTicks = 0;          // pausa do reforco de curva (anti-oscilacao)
bool g_havePost = false;
int g_preCur = 0;                 // angulo do Spinner no inicio do tick
int g_postCur = 0;                // angulo do Spinner no fim do tick anterior
float g_postSpeed = 0.0f;         // velocidade do Spinner no fim do tick anterior
daAlink_c* g_link = nullptr;      // Link (guardado no hook procSpinnerWait)

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
//  Orientacao de Link. O jogo refaz o shape_angle do Link a cada tick a partir de
//  OUTRA variavel (a direcao em que ele subiu). Varremos o objeto do Link atras
//  de outros valores de 16 bits iguais ao angulo inicial e escrevemos neles.
// ---------------------------------------------------------------------------
constexpr int kMaxCand = 24;
std::ptrdiff_t g_candOff[kMaxCand];
int g_candCount = 0;
bool g_scanDone = false;
int16_t g_faceSmooth = 0;        // angulo "suavizado" que queremos mostrar
bool g_smoothInit = false;

std::ptrdiff_t g_knownOff[kMaxCand];   // campos ja validados em giros anteriores
int g_knownCount = 0;

bool contains_off(const std::ptrdiff_t* list, int count, std::ptrdiff_t off) {
    for (int i = 0; i < count; ++i) {
        if (list[i] == off) return true;
    }
    return false;
}

void scan_link_angles(daAlink_c* link) {
    const unsigned char* base = reinterpret_cast<const unsigned char*>(link);
    const int16_t target = link->shape_angle.y;
    const std::ptrdiff_t offShape = reinterpret_cast<const unsigned char*>(&link->shape_angle.y) - base;
    const std::ptrdiff_t offCur = reinterpret_cast<const unsigned char*>(&link->current.angle.y) - base;
    const std::size_t startAt = sizeof(fopAc_ac_c);   // ignora os campos comuns de qualquer ator

    // 1) Varredura bruta: todos os campos de 16 bits iguais ao angulo inicial.
    std::ptrdiff_t raw[kMaxCand];
    int rawCount = 0;
    int rawTotal = 0;
    for (std::size_t off = startAt; off + 2 <= sizeof(daAlink_c); off += 2) {
        int16_t v;
        std::memcpy(&v, base + off, 2);
        const std::ptrdiff_t o = static_cast<std::ptrdiff_t>(off);
        if (v == target && o != offShape && o != offCur) {
            ++rawTotal;
            if (rawCount < kMaxCand) raw[rawCount++] = o;
        }
    }

    // 2) Filtro: poucos campos = varredura confiavel. Muitos = o valor e comum (ex.: -32768)
    //    e a maioria e coincidencia; nesse caso so usamos campos ja validados antes.
    g_candCount = 0;
    if (rawTotal >= 1 && rawTotal <= 8) {
        for (int i = 0; i < rawCount; ++i) {
            if (g_knownCount == 0 || contains_off(g_knownOff, g_knownCount, raw[i])) {
                g_candOff[g_candCount++] = raw[i];
            }
        }
        if (g_candCount == 0) {                       // nada em comum com o historico: recomeca
            for (int i = 0; i < rawCount; ++i) g_candOff[g_candCount++] = raw[i];
        }
        g_knownCount = g_candCount;                   // guarda os campos validados
        for (int i = 0; i < g_candCount; ++i) g_knownOff[i] = g_candOff[i];
    } else if (g_knownCount > 0) {
        for (int i = 0; i < g_knownCount; ++i) {
            int16_t v;
            std::memcpy(&v, base + g_knownOff[i], 2);
            if (v == target) g_candOff[g_candCount++] = g_knownOff[i];
        }
    }

    char buf[320];
    int n = std::snprintf(buf, sizeof(buf), "FACE-SCAN: valor %d | encontrados %d | usando %d:",
                          static_cast<int>(target), rawTotal, g_candCount);
    for (int i = 0; i < g_candCount && n > 0 && n < static_cast<int>(sizeof(buf)) - 12; ++i) {
        n += std::snprintf(buf + n, sizeof(buf) - n, " +0x%X", static_cast<unsigned>(g_candOff[i]));
    }
    svc_log->info(mod_ctx, buf);
}

// Escreve o angulo suavizado no shape_angle e nos campos candidatos.
void write_face_candidates(daAlink_c* link) {
    link->shape_angle.y = g_faceSmooth;
    unsigned char* base = reinterpret_cast<unsigned char*>(link);
    for (int i = 0; i < g_candCount; ++i) {
        const int16_t v = g_faceSmooth;
        std::memcpy(base + g_candOff[i], &v, 2);
    }
}

// Reaplica a orientacao de Link (usada nos hooks "post").
void apply_face(daAlink_c* link, daSpinner_c* spinner) {
    if (link == nullptr || spinner == nullptr) return;
    if (spinner->checkPathMoveNow() != nullptr) return;   // nos trilhos: nao mexe
    if (g_smoothInit) write_face_candidates(link);
}

// ---------------------------------------------------------------------------
//  Animacao do Spinner (mBck). Os nomes das funcoes da animacao sao detectados em
//  tempo de compilacao: se algum nao existir, simplesmente nao fazemos nada.
// ---------------------------------------------------------------------------
template <class A>
bool anim_read(A& a, float& frame, float& endFrame) {
    if constexpr (requires { a.getFrame(); a.getEndFrame(); }) {
        frame = static_cast<float>(a.getFrame());
        endFrame = static_cast<float>(a.getEndFrame());
        return true;
    } else {
        return false;
    }
}

template <class A>
bool anim_write(A& a, float frame) {
    if constexpr (requires { a.setFrame(frame); }) {
        a.setFrame(frame);
        return true;
    } else {
        return false;
    }
}

bool g_warnedNoAnimApi = false;

// Com o motor ligado, faz o loop da animacao: ao chegar no quadro final, volta ao inicial.
void anim_tick(daSpinner_c* spinner, bool motorHeld) {
    auto& bck = spinner->*steal_field(SpinnerBckTag{});
    float frame = 0.0f, endFrame = 0.0f;
    const bool ok = anim_read(bck, frame, endFrame);

    if (!ok) {
        if (!g_warnedNoAnimApi) {
            g_warnedNoAnimApi = true;
            svc_log->warn(mod_ctx, "ANIM: nao consegui ler a animacao do Spinner (API diferente)");
        }
        return;
    }

    if (motorHeld && spinner->checkPathMoveNow() == nullptr) {
        if (frame >= static_cast<float>(kLoopEndFrame)) {
            anim_write(bck, static_cast<float>(kLoopStartFrame));
        }
    }
}

// Roda ANTES de cada tick em que Link esta no Spinner.
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
        g_lastSpinner = spinner;
        g_havePost = false;   // nao compara com o giro anterior
        g_lastD = 0;
        g_noAssistTicks = 0;
        g_scanDone = false;
        g_smoothInit = false;
        svc_log->info(mod_ctx, "Novo giro de Spinner");
    }
    g_lastWait = now;

    // Varredura (uma vez por giro): procura onde o jogo guarda a direcao inicial do Link.
    if (!g_scanDone) {
        g_scanDone = true;
        scan_link_angles(link);
    }

    // Atualiza o angulo suavizado e escreve ANTES do jogo refazer o shape_angle.
    if (spinner != nullptr && spinner->checkPathMoveNow() == nullptr) {
        if (!g_smoothInit) {
            g_faceSmooth = link->shape_angle.y;
            g_smoothInit = true;
        }
        const int d = static_cast<int16_t>(spinner->current.angle.y - g_faceSmooth);
        int st = d * kFacePercentPerTick / 100;
        if (st == 0 && d != 0) st = d > 0 ? 1 : -1;
        if (st > kFaceMaxStep) st = kFaceMaxStep;
        if (st < -kFaceMaxStep) st = -kFaceMaxStep;
        g_faceSmooth = static_cast<int16_t>(g_faceSmooth + st);
        write_face_candidates(link);
    }
    return HOOK_CONTINUE;
}

// Roda DEPOIS de cada tick em que Link esta no Spinner: reaplica a orientacao de Link.
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
        g_noAssistTicks = 0;
        g_lastD = 0;
        g_havePost = false;
        return HOOK_CONTINUE;
    }

    g_preCur = static_cast<int>(spinner->current.angle.y);

    // ------------------------------------------------------------------
    //  CURVA MAIS LIVRE: o jogo gira o Spinner ate 360 unidades por tick.
    //  Medimos quanto o jogo girou neste tick e somamos um reforco na
    //  mesma direcao (kTurnPercent/100 vezes mais rapido).
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
            const int extra = static_cast<int>(static_cast<float>(d) * (static_cast<float>(kTurnPercent) / 100.0f - 1.0f));
            if (extra != 0) {
                spinner->current.angle.y = static_cast<int16_t>(spinner->current.angle.y + extra);
                spinner->shape_angle.y = static_cast<int16_t>(spinner->shape_angle.y + extra);
            }
        }
    }

    // ------------------------------------------------------------------
    //  MOTOR: segurando A, o Spinner acelera sem parar ate a velocidade maxima,
    //  e a animacao das serras fica em loop.
    // ------------------------------------------------------------------
    const bool holdA = read_hold_a<mDoCPd_c>();
    const bool holdJump = spinner->getButtonJump() != 0;   // aviso do proprio Spinner
    const bool held = holdA || holdJump;
    anim_tick(spinner, held);

    if (held) {
        const float cap = max_speed();
        const float v = spinner->speedF;
        if (v >= kMinSpeedToBoost && v < cap) {
            // Perda natural de velocidade que o jogo aplicou desde o fim do tick anterior:
            // devolvemos, para a subida ficar constante ate o maximo.
            float loss = 0.0f;
            if (g_havePost) {
                loss = g_postSpeed - v;
                if (loss < 0.0f || loss > 5.0f) loss = 0.0f;
            }
            float nv = v + loss + kMotorAccelPerSecond / 30.0f;
            if (nv > cap) nv = cap;
            spinner->speedF = nv;
        }
    }
    return HOOK_CONTINUE;
}

// Roda DEPOIS do movimento do Spinner: guarda o angulo e a velocidade finais deste tick,
// para medirmos no proximo tick quanto o jogo girou e desacelerou o Spinner.
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

// Painel do mod na janela Mods: a unica opcao ajustavel.
ModResult build_panel(ModContext*, UiElementHandle panel, void*, ModError*) {
    svc_ui->pane_add_section(mod_ctx, panel, "Spinner Speed");

    UiControlDesc maxCtl = UI_CONTROL_DESC_INIT;
    maxCtl.kind = UI_CONTROL_NUMBER;
    maxCtl.label = "Velocidade maxima";
    maxCtl.help_rml = "Velocidade maxima do Spinner com o motor (segurando A). O normal do jogo e cerca de 26. Padrao: 70.";
    maxCtl.binding = UI_BINDING_CONFIG_VAR;
    maxCtl.config_var = g_varMaxSpeed;
    maxCtl.min = 30;
    maxCtl.max = 300;
    maxCtl.step = 5;
    svc_ui->pane_add_control(mod_ctx, panel, &maxCtl, nullptr);

    return MOD_OK;
}

}  // namespace

extern "C" {

MOD_EXPORT ModResult mod_initialize(ModError* error) {
    g_lastSpinner = nullptr;
    g_loggedFirstWait = false;
    g_loggedFirstMove = false;
    g_havePost = false;
    g_lastD = 0;
    g_noAssistTicks = 0;
    g_link = nullptr;
    g_warnedNoAnimApi = false;
    g_scanDone = false;
    g_smoothInit = false;
    g_candCount = 0;
    g_knownCount = 0;

    // Opcao salva no config do Dusklight. O nome "maxSpeed70" e novo de proposito:
    // assim o valor antigo salvo nao sobrepoe o novo padrao (70).
    ConfigVarDesc maxDesc = CONFIG_VAR_DESC_INIT;
    maxDesc.name = "maxSpeed70";
    maxDesc.type = CONFIG_VAR_INT;
    maxDesc.default_int = kDefaultMaxSpeed;
    if (svc_config->register_var(mod_ctx, &maxDesc, &g_varMaxSpeed) != MOD_OK) {
        g_varMaxSpeed = 0;
        svc_log->warn(mod_ctx, "Nao foi possivel registrar a opcao de velocidade maxima (usando padrao)");
    }

    // Painel com a opcao na janela Mods (so se a opcao existe).
    if (g_varMaxSpeed != 0) {
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
