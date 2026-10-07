// ReShade overlay tab "Ray Reconstruction": live status of DLSS-RR and its guides, the settings
// (saved to ReShade.ini, see settings.cpp) and the developer tools. The hotkeys keep working:
// F11 = RR on/off; with developer hotkeys on, F7 = metalness from the albedo alpha and
// F10 / Shift+F10 / Ctrl+Shift+F10 = captures.
// Runs on the present thread; it only reads snapshots and writes rr::cfg atomics.
// Every text is given in English and Spanish (rr::tr, language selector in the General tab).

#include "common.hpp"

#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>

#include <cmath>
#include <cstdio>
#include <mutex>

using rr::tr;

namespace
{
    const ImVec4 kGreen(0.40f, 0.85f, 0.40f, 1.0f);
    const ImVec4 kYellow(0.95f, 0.75f, 0.25f, 1.0f);
    const ImVec4 kRed(0.95f, 0.35f, 0.35f, 1.0f);
    const ImVec4 kGrey(0.60f, 0.60f, 0.60f, 1.0f);

    // Preset combo: index -> sl::DLSSDPreset value.
    const int kPresetValues[] = { 6, 5, 4, 0 };
    const char *const kPresetNamesEn[] = { "F (recommended, DLSS 4.5)", "E", "D", "Driver default" };
    const char *const kPresetNamesEs[] = { "F (recomendado, DLSS 4.5)", "E", "D", "Predeterminado del driver" };

    const char *preset_name(int value)
    {
        switch (value)
        {
        case 6: return "F";
        case 5: return "E";
        case 4: return "D";
        case 0: return tr("driver default", "predeterminado del driver");
        default: return "?";
        }
    }

    // sl::DLSSMode
    const char *mode_name(int mode)
    {
        switch (mode)
        {
        case 0: return tr("off", "apagado");
        case 1: return tr("Performance", "Rendimiento");
        case 2: return tr("Balanced", "Equilibrado");
        case 3: return tr("Quality", "Calidad");
        case 4: return tr("Ultra Performance", "Ultra rendimiento");
        case 5: return tr("Ultra Quality", "Ultra calidad");
        case 6: return "DLAA";
        default: return "?";
        }
    }

    // Per-second rates from the cumulative counters, refreshed once a second.
    struct rates
    {
        uint64_t tick = 0, ok = 0, fallback = 0, fail = 0, dispatches = 0;
        double ok_s = 0, fallback_s = 0, fail_s = 0, dispatches_s = 0;

        void update(const rr::rr_snapshot &s, const rr::guides_snapshot &g)
        {
            const uint64_t now = GetTickCount64();
            if (tick == 0)
            {
                tick = now;
                ok = s.ok, fallback = s.fallback, fail = s.fail, dispatches = g.dispatches;
                return;
            }
            const uint64_t dt = now - tick;
            if (dt < 1000)
                return;
            const double k = 1000.0 / double(dt);
            ok_s = double(s.ok - ok) * k;
            fallback_s = double(s.fallback - fallback) * k;
            fail_s = double(s.fail - fail) * k;
            dispatches_s = double(g.dispatches - dispatches) * k;
            tick = now;
            ok = s.ok, fallback = s.fallback, fail = s.fail, dispatches = g.dispatches;
        }
    } g_rates;

    void help_marker(const char *text)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("[?]");
        if (ImGui::BeginItemTooltip())
        {
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 32.0f);
            ImGui::TextUnformatted(text);
            ImGui::PopTextWrapPos();
            ImGui::EndTooltip();
        }
    }

    void row(const char *label, const ImVec4 *color, const char *fmt, ...)
    {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s", label);
        ImGui::TableSetColumnIndex(1);
        if (color != nullptr)
            ImGui::TextColored(*color, "%s", buf);
        else
            ImGui::TextUnformatted(buf);
    }

    // A guide option changed: save it and restart RR's history so the old guides do not linger.
    void guide_option_changed(const char *what)
    {
        rr::cfg::save();
        rr::cfg::reset_history = true;
        rr::log_info(std::string("overlay: ") + what);
    }

    // ---- Confirmation of every setting: did the change really reach the game? ----
    // Each check compares what the menu asks for with what the code that runs on the GPU last used
    // (guide constants, RR options, pipeline swaps), so "[OK]" means applied, not just clicked.

    enum class level { ok, pending, error, info };
    struct feedback
    {
        level lv;
        std::string text;
    };

    bool fresh(uint64_t tick, uint64_t ms = 1000) { return tick != 0 && GetTickCount64() - tick < ms; }

    bool rr_running(const rr::rr_snapshot &s) { return s.state == 1 && s.enabled && fresh(s.last_rr_tick); }

    feedback fb_rr(const rr::rr_snapshot &s)
    {
        if (s.state == -1)
            return { level::error, tr("RR not available: ", "RR no disponible: ") +
                                       (s.last_error.empty() ? std::string(tr("the DLSS-RR plugin was not loaded", "el plugin DLSS-RR no se carg\xC3\xB3")) : s.last_error) };
        if (s.state == 0)
            return { level::pending, tr("waiting for Streamline to start (a few seconds of gameplay)", "esperando a que Streamline arranque (unos segundos de juego)") };
        if (!s.enabled)
            return fresh(s.last_rr_tick) ? feedback { level::pending, tr("turning off...", "apagando...") }
                                         : feedback { level::ok, tr("off: the game uses its normal DLSS", "apagado: el juego usa su DLSS normal") };
        if (rr_running(s))
            return { level::ok, tr("active: RR is processing every frame", "activo: RR est\xC3\xA1 procesando cada frame") };
        if (!s.last_error.empty() && s.fail > 0)
            return { level::error, s.last_error };
        return { level::pending, tr("waiting for the game to draw", "esperando a que el juego dibuje") + (s.last_skip.empty() ? std::string() : " (" + s.last_skip + ")") };
    }

    feedback fb_preset(const rr::rr_snapshot &s)
    {
        const int wanted = rr::cfg::preset.load();
        if (!s.enabled || s.state != 1)
            return { level::info, tr("applies when RR is active", "se aplicar\xC3\xA1 cuando RR est\xC3\xA9 activo") };
        if (s.last_error.find("slDLSSDSetOptions") != std::string::npos)
            return { level::error, s.last_error };
        if (rr_running(s) && s.active_preset == wanted)
            return { level::ok, std::string("preset ") + preset_name(wanted) + tr(" in use", " en uso") };
        return { level::pending, tr("loading preset ", "cargando el preset ") + std::string(preset_name(wanted)) + "..." };
    }

    feedback fb_sharpen(const rr::rr_snapshot &s)
    {
        if (!rr::cfg::rr_sharpen.load())
            return { level::ok, tr("off", "apagado") };
        const rr::sharpen_snapshot sh = rr::sharpen_get_snapshot();
        if (!sh.error.empty())
            return { level::error, sh.error };
        if (!rr_running(s))
            return { level::info, tr("only applies while RR is active", "se aplica solo con RR activo") };
        if (fresh(sh.last_tick) && sh.last_strength == rr::cfg::rr_sharpness.load())
            return { level::ok, tr("applied on every RR frame (", "aplicado en cada frame de RR (") + std::to_string(sh.last_strength) + " %)" };
        return { level::pending, tr("applying...", "aplicando...") };
    }

    // `applied` = the value the last guide dispatch used, `wanted` = the menu value.
    feedback fb_guide(const rr::rr_snapshot &s, const rr::guides_snapshot &g, bool applied, bool wanted, const char *effect_note = nullptr)
    {
        if (!g.pipeline_ready)
            return { level::error, tr("the guide shader could not be created (see ReShade's Log tab)",
                                      "el shader de gu\xC3\xAD" "as no se pudo crear (ver pesta\xC3\xB1" "a Log de ReShade)") };
        if (!fresh(g.last_dispatch_tick))
            return { level::pending, tr("waiting for the game to draw (menu or loading screen?)",
                                        "esperando a que el juego dibuje (\xC2\xBFmen\xC3\xBA o pantalla de carga?)") };
        if (applied != wanted)
            return { level::pending, tr("applying...", "aplicando...") };
        std::string text = wanted ? tr("on and applied on the GPU", "encendido y aplicado en la GPU") : tr("off and applied on the GPU", "apagado y aplicado en la GPU");
        if (!rr_running(s))
            return { level::info, text + tr(", but no visible effect: RR is off", ", pero sin efecto visible: RR est\xC3\xA1 apagado") };
        if (effect_note != nullptr)
            text += std::string(" (") + effect_note + ")";
        return { level::ok, text };
    }

    feedback fb_threshold(const rr::rr_snapshot &s, const rr::guides_snapshot &g)
    {
        const int wanted = rr::cfg::foliage_threshold.load();
        feedback f = fb_guide(s, g, g.applied_threshold == wanted, true);
        if (f.lv == level::ok || f.lv == level::info)
            f.text = tr("threshold ", "umbral ") + std::to_string(g.applied_threshold) + tr(" % applied on the GPU", " % aplicado en la GPU") +
                     (f.lv == level::info ? tr(", but RR is off", ", pero RR est\xC3\xA1 apagado") : "");
        return f;
    }

    // One target (temporal or spatial filter): is the selected variant created and in use?
    feedback fb_gi_variant(const rr::gi_snapshot &gi, int v, bool found)
    {
        const std::string name = rr::gi_variant_name(v);
        if (gi.variant_twins[v] == 0)
        {
            if (!found)
                return { level::pending, tr("the game has not loaded this filter yet (start driving)", "el juego a\xC3\xBAn no carg\xC3\xB3 este filtro (empieza a conducir)") };
            if (!rr::gi_variants_built())
                return { level::error, tr("variant ", "variante ") + name +
                                           tr(" not in this build (built without game shaders, see the Developer tab)",
                                              " no incluida en esta compilaci\xC3\xB3n (compilada sin los shaders del juego, ver pesta\xC3\xB1" "a Desarrollo)") };
            return { level::error, tr("variant ", "variante ") + name + tr(" not created", " no creada") +
                                       (gi.last_failure.empty() ? std::string(tr(" (see Log)", " (ver Log)")) : ": " + gi.last_failure) };
        }
        if (fresh(gi.variant_tick[v]))
            return { level::ok, tr("variant ", "variante ") + name + tr(" in use", " en uso") };
        return { level::pending, tr("variant ", "variante ") + name +
                                     tr(" ready, waiting for the game to use it (menu or loading?)", " lista, esperando a que el juego la use (\xC2\xBFmen\xC3\xBA o carga?)") };
    }

    feedback fb_gi(const rr::rr_snapshot &s, const rr::gi_snapshot &gi)
    {
        const int mode = rr::cfg::gi_temporal_mode.load();
        if (mode == 0)
            return { level::ok, tr("original: the game unchanged", "original: el juego sin cambios") };
        if (!gi.hook_installed)
            return { level::pending, tr("the pipeline hook is installed after a few seconds of gameplay", "el hook de pipelines se instala tras unos segundos de juego") };
        if (mode >= 3 && !rr_running(s))
            return { level::info, tr("inactive: this mode only acts with RR on; the original is used now",
                                     "inactivo: este modo solo act\xC3\xBA" "a con RR encendido; ahora se usa el original") };
        feedback t = gi.want_temporal >= 0 ? fb_gi_variant(gi, gi.want_temporal, gi.temporal_found) : feedback { level::ok, "" };
        if (t.lv != level::ok)
            return { t.lv, tr("temporal filter: ", "filtro temporal: ") + t.text };
        if (gi.want_spatial >= 0)
        {
            const feedback sp = fb_gi_variant(gi, gi.want_spatial, gi.spatial_found);
            if (sp.lv != level::ok)
                return { sp.lv, tr("spatial filter: ", "filtro espacial: ") + sp.text };
            return { level::ok, tr("applied: temporal ", "aplicado: temporal ") + std::string(rr::gi_variant_name(gi.want_temporal)) +
                                    tr(" + spatial ", " + espacial ") + rr::gi_variant_name(gi.want_spatial) };
        }
        return { level::ok, tr("applied: temporal ", "aplicado: temporal ") + std::string(rr::gi_variant_name(gi.want_temporal)) };
    }

    void status_line(const feedback &f)
    {
        const ImVec4 *color = f.lv == level::ok ? &kGreen : f.lv == level::error ? &kRed : f.lv == level::pending ? &kYellow : &kGrey;
        const char *tag = f.lv == level::ok ? "[OK]" : f.lv == level::error ? "[ERROR]" : f.lv == level::pending ? "[...]" : "[i]";
        ImGui::Indent();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(*color, "%s %s", tag, f.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Unindent();
    }

    void draw_status(const rr::rr_snapshot &s, const rr::guides_snapshot &g)
    {
        if (s.state == -1)
            ImGui::TextColored(kRed, "%s", tr("Ray Reconstruction not available: the DLSS-RR plugin was not loaded (see Log)",
                                              "Ray Reconstruction no disponible: el plugin DLSS-RR no se carg\xC3\xB3 (ver Log)"));
        else if (s.state == 0)
            ImGui::TextColored(kYellow, "%s", tr("Waiting for Streamline (hooks after ~120 frames of gameplay)", "Esperando a Streamline (se engancha tras ~120 frames de juego)"));
        else if (!s.enabled)
            ImGui::TextColored(kGrey, "%s", tr("Disabled: the game uses its normal DLSS (DLSS-SR)", "Desactivado: el juego usa su DLSS normal (DLSS-SR)"));
        else if (s.evaluating && rr::cfg::diag_neutral.load())
            ImGui::TextColored(kYellow, "%s", tr("Ray Reconstruction active  -  DIAGNOSTIC: neutral guides", "Ray Reconstruction activo  -  DIAGNÓSTICO: guí" "as neutras"));
        else if (s.evaluating)
            ImGui::TextColored(kGreen, "%s", tr("Ray Reconstruction active", "Ray Reconstruction activo"));
        else
            ImGui::TextColored(kYellow, "%s%s", tr("Fallback to DLSS-SR: ", "Respaldo a DLSS-SR: "),
                               s.last_skip.empty() ? (s.last_error.empty() ? tr("no reason recorded", "sin motivo registrado") : s.last_error.c_str()) : s.last_skip.c_str());

        if (rr::bench_running())
            ImGui::TextColored(kYellow, "%s", tr("Measurement running: the add-on is changing settings by itself; do not move the camera.",
                                                 "Medici\xC3\xB3n en curso: el addon est\xC3\xA1 cambiando ajustes solo; no muevas la c\xC3\xA1mara."));
        if (!ImGui::BeginTable("##rr_status", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            return;
        ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch, 0.55f);

        const ImVec4 *mode_color = s.mode == 0 ? &kYellow : nullptr;
        row(tr("Game's DLSS mode", "Modo DLSS del juego"), mode_color, "%s", mode_name(s.mode));
        if (s.render_w != 0)
            row(tr("Resolution", "Resoluci\xC3\xB3n"), nullptr, "%ux%u -> %ux%u", s.render_w, s.render_h, s.out_w, s.out_h);
        else
            row(tr("Resolution", "Resoluci\xC3\xB3n"), &kGrey, "%s", tr("no data yet", "sin datos todav\xC3\xAD" "a"));
        row(tr("RR preset", "Preset de RR"), nullptr, "%s", s.ok > 0 ? preset_name(s.active_preset) : preset_name(rr::cfg::preset.load()));
        const bool vram = s.last_result == "Result::eWarnOutOfVRAM";
        row(tr("Last result", "\xC3\x9Altimo resultado"), vram ? &kYellow : (s.last_result == "Result::eOk" ? &kGreen : nullptr), "%s%s",
            s.last_result.c_str(), vram ? tr("  (VRAM at the limit)", "  (VRAM al l\xC3\xADmite)") : "");
        if (vram && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", tr("NGX warns that the VRAM is almost full. RR keeps working,\nbut it is the same pressure that makes Dynamic MFG get rejected.",
                                       "NGX avisa de que la VRAM est\xC3\xA1 casi llena. RR sigue funcionando,\npero es la misma presi\xC3\xB3n que hace rechazar el MFG Din\xC3\xA1mico."));
        row(tr("Frames with RR", "Frames con RR"), nullptr, "%llu  (%.0f/s)", (unsigned long long)s.ok, g_rates.ok_s);
        row(tr("Fallbacks to DLSS-SR", "Respaldos a DLSS-SR"), nullptr, "%llu  (%.0f/s)", (unsigned long long)s.fallback, g_rates.fallback_s);
        row(tr("Failures", "Fallos"), s.fail > 0 ? &kRed : nullptr, "%llu  (%.0f/s)", (unsigned long long)s.fail, g_rates.fail_s);
        row(tr("History resets", "Reinicios de historial"), nullptr, "%llu", (unsigned long long)s.resets);
        if (g.width != 0)
            row(tr("Guides", "Gu\xC3\xAD" "as"), nullptr, "%ux%u, %.0f/s", g.width, g.height, g_rates.dispatches_s);
        else
            row(tr("Guides", "Gu\xC3\xAD" "as"), &kGrey, "%s", g.pipeline_ready ? tr("waiting for the G-buffer", "esperando el G-buffer") : tr("shader not created", "shader sin crear"));
        row(tr("Input colour", "Color de entrada"), nullptr, "%s, preExposure %.3g%s", s.hdr ? "HDR" : "SDR", s.pre_exposure,
            s.auto_exposure ? tr(", auto exposure", ", autoexposici\xC3\xB3n") : "");
        if (!s.last_error.empty())
            row(tr("Last error", "\xC3\x9Altimo error"), &kRed, "%s", s.last_error.c_str());
        ImGui::EndTable();
    }

    void draw_general(const rr::rr_snapshot &s)
    {
        static const char *const kLanguages[] = { "English", "Espa\xC3\xB1ol" };
        int language = rr::cfg::language.load();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 10.0f);
        if (ImGui::Combo("Language / Idioma", &language, kLanguages, 2, -1))
        {
            rr::cfg::language = language;
            rr::cfg::save();
        }
        ImGui::Spacing();

        bool enabled = rr::cfg::rr_enabled.load();
        ImGui::BeginDisabled(s.state != 1);
        if (ImGui::Checkbox("Ray Reconstruction (F11)", &enabled))
            rr::rr_toggle();
        ImGui::EndDisabled();
        help_marker(tr("On: DLSS Ray Reconstruction replaces the game's DLSS and cleans the ray tracing noise.\n"
                       "Off: the game uses its normal DLSS. Use it to compare (same as F11).",
                       "Activado: DLSS Ray Reconstruction sustituye al DLSS del juego y limpia el ruido del RT.\n"
                       "Desactivado: el juego usa su DLSS normal. \xC3\x9Asalo para comparar (igual que F11)."));
        status_line(fb_rr(s));

        int index = 0;
        for (int i = 0; i < 4; ++i)
            if (kPresetValues[i] == rr::cfg::preset.load())
                index = i;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::Combo("Preset", &index, rr::lang_es() ? kPresetNamesEs : kPresetNamesEn, 4, -1))
        {
            rr::cfg::preset = kPresetValues[index];
            rr::cfg::save();
        }
        help_marker(tr("DLSS-RR model. F is the second-generation transformer of DLSS 4.5 (recommended).\n"
                       "Changing it restarts RR's history; NGX may take a moment to load the new model.",
                       "Modelo de DLSS-RR. F es el transformer de segunda generaci\xC3\xB3n de DLSS 4.5 (recomendado).\n"
                       "Cambiarlo reinicia el historial de RR; NGX puede tardar un instante en cargar el modelo nuevo."));
        status_line(fb_preset(s));


        ImGui::Spacing();
        bool sharpen = rr::cfg::rr_sharpen.load();
        if (ImGui::Checkbox(tr("RR sharpening", "Afinado de RR"), &sharpen))
        {
            rr::cfg::rr_sharpen = sharpen;
            rr::cfg::save();
            rr::log_info(std::string("overlay: RR sharpening ") + (sharpen ? "ON" : "OFF"));
        }
        help_marker(tr("RR keeps the structure of the image but softens the finest texture: the measurement gave ~12 % less\n"
                       "fine detail than normal DLSS at every distance. This adaptive sharpening (in the style of AMD CAS) brings it back:\n"
                       "it only enhances where there is room, creates no halos and never leaves the range of the neighbouring pixels.\n"
                       "It is applied to RR's HDR output, before the tonemap and the HUD. Only with RR active. Cost: two light passes.",
                       "RR conserva la estructura de la imagen pero suaviza la textura m\xC3\xA1s fina: la medici\xC3\xB3n dio ~12 % menos\n"
                       "detalle fino que el DLSS normal a todas las distancias. Este afinado adaptativo (estilo CAS de AMD) lo devuelve:\n"
                       "realza solo donde hay margen, no crea halos y nunca sale del rango de los p\xC3\xADxeles vecinos.\n"
                       "Se aplica a la salida HDR de RR, antes del tonemap y del HUD. Solo con RR activo. Coste: dos pases ligeros."));
        ImGui::Indent();
        ImGui::BeginDisabled(!sharpen);
        int strength = rr::cfg::rr_sharpness.load();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::SliderInt(tr("Sharpening strength", "Intensidad del afinado"), &strength, 0, 100, "%d %%", 0))
            rr::cfg::rr_sharpness = strength;
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            rr::cfg::save();
            rr::log_info("overlay: RR sharpness " + std::to_string(strength) + "%");
        }
        help_marker(tr("Measured (4 Oct 2026): 30 % brings the fine detail back to ~1.04-1.07 of normal DLSS without more noise;\n"
                       "60 % and 90 % overshoot (and 90 % already adds noise). Default 30 %.",
                       "Medido (4 oct 2026): 30 % devuelve el detalle fino a ~1,04-1,07 del DLSS normal sin subir el ruido;\n"
                       "60 % y 90 % se pasan (y 90 % ya sube el ruido). Por defecto 30 %."));
        ImGui::EndDisabled();
        status_line(fb_sharpen(s));
        ImGui::Unindent();

        if (ImGui::Button(tr("Reset RR history", "Reiniciar historial de RR")))
            rr::cfg::reset_history = true;
        help_marker(tr("Drops RR's temporal accumulation on the next frame.\n"
                       "Useful if a trail or a stuck highlight remains after a sudden change.",
                       "Descarta la acumulaci\xC3\xB3n temporal de RR en el siguiente frame.\n"
                       "\xC3\x9Atil si queda una estela o un brillo pegado tras un cambio brusco."));
    }

    void draw_guides(const rr::rr_snapshot &s, const rr::guides_snapshot &g)
    {
        ImGui::TextWrapped("%s", tr("The guides tell RR the colour, normal and roughness of every surface. "
                                    "Forza does not provide them: they are built from its RT G-buffer.",
                                    "Las gu\xC3\xAD" "as le dicen a RR qu\xC3\xA9 color, normal y rugosidad tiene cada superficie. "
                                    "Forza no las da: se construyen a partir de su G-buffer de RT."));
        ImGui::Spacing();


        bool foliage = rr::cfg::foliage_neutral.load();
        if (ImGui::Checkbox(tr("Neutral foliage (prevents burnt leaves)", "Follaje neutro (evita hojas quemadas)"), &foliage))
        {
            rr::cfg::foliage_neutral = foliage;
            guide_option_changed(foliage ? "foliage neutral ON" : "foliage neutral OFF");
        }
        help_marker(tr("The RT G-buffer has simplified foliage that does not match the one on screen.\n"
                       "Where the normal is noise (leaves), the base colour becomes white so that RR does not divide\n"
                       "the sky by an almost black green, which burnt the leaves.",
                       "El G-buffer de RT tiene un follaje simplificado que no coincide con el de pantalla.\n"
                       "Donde la normal es ruido (hojas), el color base pasa a blanco para que RR no divida\n"
                       "el cielo entre un verde casi negro, que quemaba las hojas."));
        status_line(fb_guide(s, g, (g.applied_flags & 2u) != 0, foliage));

        ImGui::Indent();
        ImGui::BeginDisabled(!foliage);
        int treatment = rr::cfg::foliage_floor_mode.load() ? 1 : 0;
        static const char *const kTreatmentsEn[] = { "White (maximum protection)", "Base colour floor (keeps detail)" };
        static const char *const kTreatmentsEs[] = { "Blanco (m\xC3\xA1xima protecci\xC3\xB3n)", "Piso de color base (conserva el detalle)" };
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::Combo(tr("Foliage treatment", "Tratamiento del follaje"), &treatment, rr::lang_es() ? kTreatmentsEs : kTreatmentsEn, 2, -1))
        {
            rr::cfg::foliage_floor_mode = treatment == 1;
            guide_option_changed(treatment == 1 ? "foliage treatment: albedo floor" : "foliage treatment: white");
        }
        help_marker(tr("White: RR stops dividing by the colour on foliage. Maximum protection against burnt leaves, but the texture\n"
                       "detail moves into the lighting signal and RR smooths it (fences and far patterns lose definition).\n"
                       "Floor: only raises the darkest colours up to the floor (what caused the burning); bright colours keep\n"
                       "their real colour and their sharp texture.",
                       "Blanco: RR deja de dividir por el color en el follaje. Protege al m\xC3\xA1ximo de hojas quemadas, pero el detalle\n"
                       "de la textura pasa a la se\xC3\xB1" "al de luz y RR lo suaviza (vallas y patrones lejanos pierden definici\xC3\xB3n).\n"
                       "Piso: solo sube los colores m\xC3\xA1s oscuros hasta el piso (lo que provocaba la quemadura); lo claro conserva\n"
                       "su color real y su textura n\xC3\xADtida."));
        status_line(fb_guide(s, g, (g.applied_flags & 32u) != 0, treatment == 1));
        if (treatment == 1)
        {
            int albedo_floor = rr::cfg::foliage_floor.load();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
            if (ImGui::SliderInt(tr("Base colour floor", "Piso de color base"), &albedo_floor, 10, 40, "%d %%", 0))
                rr::cfg::foliage_floor = albedo_floor;
            if (ImGui::IsItemDeactivatedAfterEdit())
                guide_option_changed(("albedo floor " + std::to_string(albedo_floor) + "%").c_str());
            help_marker(tr("Higher = less risk of burnt leaves, a bit less detail on dark foliage. Default 20 %.",
                           "M\xC3\xA1s alto = menos riesgo de hojas quemadas, algo menos de detalle en follaje oscuro. Por defecto 20 %."));
            status_line(fb_guide(s, g, int((g.applied_flags >> 16) & 255u) == rr::cfg::foliage_floor.load(), true));
        }
        ImGui::EndDisabled();
        ImGui::Unindent();

        ImGui::Indent();
        ImGui::BeginDisabled(!foliage);
        int threshold = rr::cfg::foliage_threshold.load();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::SliderInt(tr("Foliage threshold", "Umbral de follaje"), &threshold, 50, 95, "%d %%", 0))
            rr::cfg::foliage_threshold = threshold;
        if (ImGui::IsItemDeactivatedAfterEdit())
            guide_option_changed(("foliage threshold " + std::to_string(threshold) + "%").c_str());
        help_marker(tr("Mean coherence of the normal with its 4 neighbours below which a pixel counts as foliage.\n"
                       "Higher = more pixels treated as foliage (fewer burnt leaves, slightly less noise cleaning on them).\n"
                       "Lower = only the noisiest foliage. Default 95 %.",
                       "Coherencia media de la normal con sus 4 vecinos por debajo de la cual un p\xC3\xADxel cuenta como follaje.\n"
                       "M\xC3\xA1s alto = m\xC3\xA1s p\xC3\xADxeles tratados como follaje (menos hojas quemadas, algo menos de limpieza de ruido en ellas).\n"
                       "M\xC3\xA1s bajo = solo el follaje m\xC3\xA1s ruidoso. Por defecto 95 %."));
        if (foliage)
            status_line(fb_threshold(s, g));
        bool dilate = rr::cfg::foliage_dilate.load();
        if (ImGui::Checkbox(tr("Widen the foliage mask (2 px radius)", "Ampliar máscara de follaje (radio 2 px)"), &dilate))
        {
            rr::cfg::foliage_dilate = dilate;
            guide_option_changed(dilate ? "foliage dilation ON" : "foliage dilation OFF");
        }
        help_marker(tr("Many leaves are flat cards with a uniform normal inside, which the 4-neighbour detector does not flag.\n"
                       "Off by default: it also flags fences and far detail, which then lose definition with RR.\n"
                       "With this, a pixel counts as foliage if the normals within a 2-pixel radius disagree.\n"
                       "Side effect: a 2-pixel band along strong edges is also treated as foliage.",
                       "Muchas hojas son planos (tarjetas) con normal uniforme por dentro, que el detector de 4 vecinos no marca.\n"
                       "Apagado por defecto: tambi\xC3\xA9n marca vallas y detalle lejano, que entonces pierde definici\xC3\xB3n con RR.\n"
                       "Con esto, un píxel cuenta como follaje si en un radio de 2 píxeles las normales no concuerdan.\n"
                       "Efecto secundario: una franja de 2 píxeles en bordes marcados también se trata como follaje."));
        if (foliage)
            status_line(fb_guide(s, g, (g.applied_flags & 8u) != 0, dilate));
        ImGui::EndDisabled();
        ImGui::Unindent();

        bool specular = rr::cfg::foliage_specular.load();
        if (ImGui::Checkbox(tr("Fade specular on foliage", "Atenuar especular en follaje"), &specular))
        {
            rr::cfg::foliage_specular = specular;
            guide_option_changed(specular ? "foliage specular fade ON" : "foliage specular fade OFF");
        }
        help_marker(tr("Lowers the specular brightness of the guide where the normal is noise.\n"
                       "Removes the silver dots RR left on leaves.",
                       "Baja el brillo especular de la gu\xC3\xAD" "a donde la normal es ruido.\n"
                       "Quita los puntitos plateados que RR dejaba en las hojas."));
        status_line(fb_guide(s, g, (g.applied_flags & 4u) != 0, specular));

        bool metal = rr::cfg::metal_alpha.load();
        if (ImGui::Checkbox(tr("Metalness from the albedo alpha (F7, experimental)", "Metalness desde el alfa del albedo (F7, experimental)"), &metal))
            rr::guides_metal_toggle();
        help_marker(tr("The alpha channel of the game's albedo looks like metalness (grey on paint, white on chrome),\n"
                       "but it is not confirmed. Off = everything dielectric (F0 = 0.04).\n"
                       "Subtle effect: it only changes how RR treats reflections on chrome and metallic paint.",
                       "El canal alfa del albedo del juego parece ser metalness (gris en la pintura, blanco en cromados),\n"
                       "pero no est\xC3\xA1 confirmado. Apagado = todo diel\xC3\xA9" "ctrico (F0 = 0,04).\n"
                       "Efecto sutil: solo cambia c\xC3\xB3mo RR trata los reflejos de cromados y pintura metalizada."));
        status_line(fb_guide(s, g, g.applied_metal != 0, rr::cfg::metal_alpha.load(),
                             tr("subtle effect: only reflections on metal parts", "efecto sutil: solo reflejos de partes met\xC3\xA1licas")));

        ImGui::SeparatorText(tr("Reflections", "Reflejos"));
        bool smv = rr::cfg::rr_spec_mv.load();
        if (ImGui::Checkbox(tr("Specular motion vectors (experimental)", "Vectores de movimiento especulares (experimental)"), &smv))
        {
            rr::cfg::rr_spec_mv = smv;
            rr::cfg::save();
            rr::cfg::reset_history = true;
            rr::log_info(std::string("overlay: specular motion vectors ") + (smv ? "on" : "off"));
        }
        help_marker(tr("Tells RR how reflections move, which is not how the surface they appear on moves. Without them RR\n"
                       "reprojects a reflection with the surface's motion, throws its history away and leaves it noisy.\n"
                       "Forza does not store how far each reflection is, so a fixed distance is used, scaled by how glossy the\n"
                       "surface is (gloss\xC2\xB2). Method adapted from speedlemur's Control Ray Reconstruction (MIT).\n"
                       "Judge it while driving: glass, puddles and glossy paint. The bench cannot measure it (still camera).",
                       "Le dice a RR c\xC3\xB3mo se mueven los reflejos, que no es como se mueve la superficie donde aparecen. Sin esto RR\n"
                       "reproyecta el reflejo con el movimiento de la superficie, descarta su historial y lo deja con ruido.\n"
                       "Forza no guarda a qu\xC3\xA9 distancia est\xC3\xA1 cada reflejo, as\xC3\xAD que se usa una distancia fija, escalada por lo\n"
                       "brillante que sea la superficie (gloss\xC2\xB2). M\xC3\xA9todo adaptado de Control Ray Reconstruction de speedlemur (MIT).\n"
                       "Ju\xC3\xBAzgalo conduciendo: vidrios, charcos y pintura brillante. El banco no puede medirlo (c\xC3\xA1mara quieta)."));
        ImGui::Indent();
        ImGui::BeginDisabled(!smv);
        int smv_distance = rr::cfg::rr_spec_mv_distance.load();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::SliderInt(tr("Reflection distance (m)", "Distancia del reflejo (m)"), &smv_distance, 1, 500, "%d m", ImGuiSliderFlags_Logarithmic))
            rr::cfg::rr_spec_mv_distance = smv_distance;
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            rr::cfg::save();
            rr::log_info("overlay: specular motion distance " + std::to_string(smv_distance) + " m");
        }
        help_marker(tr("How far, on average, the reflected objects are. Short (5-15 m): nearby cars and walls. Long (100+ m):\n"
                       "landscape and sky. Default 30 m.",
                       "A qu\xC3\xA9 distancia est\xC3\xA1n, en promedio, los objetos reflejados. Corta (5-15 m): carros y paredes cercanos.\n"
                       "Larga (100+ m): paisaje y cielo. Por defecto 30 m."));
        ImGui::EndDisabled();
        {
            const rr::specmv_snapshot sm = rr::specmv_get_snapshot();
            feedback f { level::ok, tr("off", "apagado") };
            if (smv)
            {
                if (!sm.error.empty())
                    f = { level::error, sm.error };
                else if (!rr_running(s))
                    f = { level::info, tr("only applies while RR is active", "se aplica solo con RR activo") };
                else if (!sm.skip.empty())
                    f = { level::pending, tr("not computed: ", "sin calcular: ") + sm.skip };
                else if (fresh(sm.last_tick) && s.smv_tagged > 0 && sm.last_distance == rr::cfg::rr_spec_mv_distance.load())
                    f = { level::ok, tr("applied: ", "aplicado: ") + std::to_string(s.smv_tagged) + tr(" RR evaluations with specular motion, ", " evaluaciones de RR con movimiento especular, ") +
                                         std::to_string(sm.last_distance) + " m" };
                else
                    f = { level::pending, tr("applying...", "aplicando...") };
            }
            status_line(f);
        }
        ImGui::Unindent();

        ImGui::SeparatorText(tr("Diagnostics", "Diagnóstico"));
        bool diag = rr::cfg::diag_neutral.load();
        const bool highlighted = diag;
        if (highlighted)
            ImGui::PushStyleColor(ImGuiCol_Text, kYellow);
        if (ImGui::Checkbox(tr("Neutral guides over the whole image (for comparison only)", "Guí" "as neutras en toda la imagen (solo para comparar)"), &diag))
        {
            rr::cfg::diag_neutral = diag;
            rr::cfg::reset_history = true;
            rr::log_info(diag ? "overlay: diagnostic neutral guides ON" : "overlay: diagnostic neutral guides OFF");
        }
        if (highlighted)
            ImGui::PopStyleColor(1);
        help_marker(tr("RR stops using the surface colours over the whole image.\n"
                       "If the leaves stop burning with this, the guides are the cause; if they still burn, it is RR itself.\n"
                       "Not saved: it turns off when the game restarts.",
                       "RR deja de usar los colores de superficie en toda la imagen.\n"
                       "Si con esto las hojas dejan de quemarse, la causa son las guías; si siguen quemadas, es el propio RR.\n"
                       "No se guarda: se apaga al reiniciar el juego."));
        status_line(fb_guide(s, g, (g.applied_flags & 16u) != 0, rr::cfg::diag_neutral.load()));

        ImGui::Spacing();
        if (ImGui::Button(tr("Restore defaults", "Restaurar valores por defecto")))
        {
            rr::cfg::restore_defaults();
            rr::log_info("overlay: settings restored to defaults");
        }
        help_marker(tr("Restores every image setting of the add-on (all tabs) to its default. The language is kept.",
                       "Restaura todos los ajustes de imagen del add-on (todas las pesta\xC3\xB1" "as). El idioma se conserva."));
    }

    const char *const kGiModesEn[] = {
        "Game original",
        "History x2 (more stable)",
        "History x4 (recommended, default)",
        "No temporal filter (experimental)",
        "Full RR (experimental)",
    };
    const char *const kGiModesEs[] = {
        "Original del juego",
        "Historial x2 (m\xC3\xA1s estable)",
        "Historial x4 (recomendado, por defecto)",
        "Sin filtro temporal (experimental)",
        "RR completo (experimental)",
    };

    void draw_gi(const rr::rr_snapshot &s)
    {
        ImGui::TextWrapped("%s", tr("Controls the filters of the game's ray traced global illumination (RTGI), which cause the "
                                    "\"boiling\" blotches in tunnels and next to small lights. The modes use the game's own shader "
                                    "with a single instruction changed, or a replacement that mirrors its structure.",
                                    "Controla los filtros de la iluminaci\xC3\xB3n global por RT (RTGI) del juego, causantes del "
                                    "\"hervor\" de manchas en t\xC3\xBAneles y junto a luces peque\xC3\xB1" "as. Los modos usan el propio shader del "
                                    "juego con una sola instrucci\xC3\xB3n cambiada, o un sustituto que replica su estructura."));
        if (!rr::gi_variants_built())
            ImGui::TextColored(kYellow, "%s", tr("This build has no game shaders (game-shaders/ was empty when it was compiled): only the original mode works. "
                                                 "Use the official release, or export the shaders from the Developer tab and rebuild.",
                                                 "Esta compilaci\xC3\xB3n no tiene los shaders del juego (game-shaders/ estaba vac\xC3\xAD" "a al compilar): solo funciona el modo original. "
                                                 "Usa la versi\xC3\xB3n oficial, o exporta los shaders desde la pesta\xC3\xB1" "a Desarrollo y vuelve a compilar."));
        ImGui::Spacing();

        int mode = rr::cfg::gi_temporal_mode.load();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20.0f);
        if (ImGui::Combo(tr("Filter mode", "Modo del filtro"), &mode, rr::lang_es() ? kGiModesEs : kGiModesEn, 5, -1))
        {
            rr::cfg::gi_temporal_mode = mode;
            rr::cfg::save();
            rr::cfg::reset_history = true;
            rr::log_info("overlay: GI temporal mode " + std::to_string(mode));
        }
        help_marker(tr("Original: the game untouched.\n"
                       "History x2 / x4: the filter accumulates 2 or 4 times more frames. Less boiling, but the indirect light\n"
                       "reacts more slowly: possible trails on moving shadows and on lights that switch on.\n"
                       "History x4 is the recommended default: the most stable image, with good quality.\n"
                       "No temporal filter: the GI arrives with fresh per-frame noise for RR to clean.\n"
                       "Full RR: without the game's temporal and spatial filters; RR does all the work. The best image by day,\n"
                       "but with open problems: noise in motion, fireflies at night, noisy mirror reflections.\n"
                       "The last two only act with Ray Reconstruction active; without RR the original is used.",
                       "Original: el juego sin tocar.\n"
                       "Historial x2 / x4: el filtro acumula 2 o 4 veces m\xC3\xA1s frames. Menos hervor, pero la luz indirecta\n"
                       "reacciona m\xC3\xA1s lento: posible estela en sombras que se mueven y luces que se encienden.\n"
                       "Historial x4 es el recomendado por defecto: la imagen m\xC3\xA1s estable, con buena calidad.\n"
                       "Sin filtro temporal: el GI llega con ruido fresco de cada frame para que RR lo limpie.\n"
                       "RR completo: sin los filtros temporal y espacial del juego; RR hace todo el trabajo. La mejor imagen de d\xC3\xAD" "a,\n"
                       "pero con problemas abiertos: ruido en movimiento, destellos de noche, reflejos espejo con ruido.\n"
                       "Los dos \xC3\xBAltimos solo act\xC3\xBA" "an con Ray Reconstruction activo; sin RR se usa el original."));

        const rr::gi_snapshot g = rr::gi_get_snapshot();
        status_line(fb_gi(s, g));

        if (mode == 4)
        {
            ImGui::Indent();
            static const char *const kLevelsEn[] = { "Off", "Soft (k = 4)", "Medium (k = 2.5)", "Strong (k = 1.6)", "Neighbour range (old)" };
            static const char *const kLevelsEs[] = { "Apagada", "Suave (k = 4)", "Media (k = 2,5)", "Fuerte (k = 1,6)", "Rango de vecinos (antigua)" };
            int firefly = rr::cfg::gi_firefly.load();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
            if (ImGui::Combo(tr("Firefly suppression v2", "Supresi\xC3\xB3n de destellos v2"), &firefly, rr::lang_es() ? kLevelsEs : kLevelsEn, 5, -1))
            {
                rr::cfg::gi_firefly = firefly;
                rr::cfg::save();
                rr::cfg::reset_history = true;
                rr::log_info("overlay: GI firefly suppression v2 " + std::to_string(firefly));
            }
            help_marker(tr("Against the dots that appear at night on walls lit indirectly in Full RR.\n"
                           "v2 only looks at the GI luminance in a 5x5 window and, if a sample exceeds k times the mean of its neighbours,\n"
                           "lowers it to that limit without touching the colour. Measured to darken the image (-0.3 EV): off by default.\n"
                           "Ignored while \"Disocclusion assist\" is on (both replace the same filter).",
                           "Contra los puntos que aparecen de noche en paredes con luz indirecta en RR completo.\n"
                           "v2 mira solo la luminancia del GI en una ventana de 5x5 y, si una muestra supera k veces la media de sus vecinas,\n"
                           "la baja a ese l\xC3\xADmite sin tocar el color. Medida: oscurece la imagen (-0,3 EV); apagada por defecto.\n"
                           "Se ignora con \"Desoclusi\xC3\xB3n asistida\" activa (las dos sustituyen al mismo filtro)."));
            if (g.want_spatial >= 0)
                status_line(fb_gi_variant(g, g.want_spatial, g.spatial_found));

            static const char *const kClampsEn[] = { "Off", "Soft (k = 4)", "Medium (k = 2)", "Strong (k = 1)" };
            static const char *const kClampsEs[] = { "Apagado", "Suave (k = 4)", "Medio (k = 2)", "Fuerte (k = 1)" };
            int clamp = rr::cfg::gi_raw_clamp.load();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
            if (ImGui::Combo(tr("Isolated sample clamp", "Recorte de muestra aislada"), &clamp, rr::lang_es() ? kClampsEs : kClampsEn, 4, -1))
            {
                rr::cfg::gi_raw_clamp = clamp;
                rr::cfg::save();
                rr::cfg::reset_history = true;
                rr::log_info("overlay: GI raw sample clamp level " + std::to_string(clamp));
            }
            help_marker(tr("The game blends every raw indirect light sample with its 8 neighbours (3x3) before anything else:\n"
                           "a lone, very bright ray comes out of it as a blotch of several pixels that neither RR nor the v2 clamp can separate.\n"
                           "This clamp acts BEFORE that blend: if the brightest of the 9 samples exceeds the second by k times,\n"
                           "it is lowered to that limit (colour intact). A firefly is a single sample; real light usually covers several.\n"
                           "Measured best: k = 4 (default).",
                           "El juego mezcla cada muestra cruda de luz indirecta con sus 8 vecinas (3x3) antes de todo lo dem\xC3\xA1s:\n"
                           "un rayo suelto muy brillante sale de ah\xC3\xAD como una mancha de varios p\xC3\xADxeles que ni RR ni el recorte v2 separan.\n"
                           "Este recorte act\xC3\xBA" "a ANTES de esa mezcla: si la muestra m\xC3\xA1s brillante de las 9 supera k veces a la segunda,\n"
                           "se baja a ese l\xC3\xADmite (color intacto). Un destello es una muestra sola; la luz real suele cubrir varias.\n"
                           "Lo mejor medido: k = 4 (por defecto)."));
            if (g.want_temporal >= 0)
                status_line(fb_gi_variant(g, g.want_temporal, g.temporal_found));

            static const char *const kAssistsEn[] = { "Off", "Fades out over 4 frames", "Fades out over 8 frames" };
            static const char *const kAssistsEs[] = { "Apagada", "Se retira en 4 frames", "Se retira en 8 frames" };
            int assist = rr::cfg::gi_assist.load();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
            if (ImGui::Combo(tr("Disocclusion assist", "Desoclusi\xC3\xB3n asistida"), &assist, rr::lang_es() ? kAssistsEs : kAssistsEn, 3, -1))
            {
                rr::cfg::gi_assist = assist;
                rr::cfg::save();
                rr::cfg::reset_history = true;
                rr::log_info("overlay: GI disocclusion assist " + std::to_string(assist));
            }
            help_marker(tr("Against the raw noise when moving or when something comes back on screen: those pixels have no history\n"
                           "and RR needs a few frames to clean them. With this option the game's spatial filter comes back ONLY on pixels\n"
                           "that just appeared (the game counts the frames of every pixel) and fades out over 4 or 8 frames;\n"
                           "pixels that already have history keep reaching RR raw. Replaces firefly suppression v2.\n"
                           "Measured: removes ~25-30 % of the disocclusion noise; 8 frames was best at night.",
                           "Contra el ruido crudo al moverse o cuando algo vuelve a entrar en pantalla: esos p\xC3\xADxeles no tienen historial\n"
                           "y RR tarda unos frames en limpiarlos. Con esta opci\xC3\xB3n el filtro espacial del juego vuelve SOLO en p\xC3\xADxeles\n"
                           "reci\xC3\xA9n aparecidos (el juego lleva la cuenta de frames de cada p\xC3\xADxel) y se retira en 4 u 8 frames;\n"
                           "lo que ya lleva historial sigue llegando crudo a RR. Sustituye a la supresi\xC3\xB3n de destellos v2.\n"
                           "Medido: quita ~25-30 % del ruido de desoclusi\xC3\xB3n; de noche 8 frames fue lo mejor."));

            bool motion = rr::cfg::gi_motion_auto.load();
            if (ImGui::Checkbox(tr("GI history x4 while moving (automatic)", "GI historial x4 en movimiento (autom\xC3\xA1tico)"), &motion))
            {
                rr::cfg::gi_motion_auto = motion;
                rr::cfg::save();
                rr::log_info(std::string("overlay: GI motion hand-over ") + (motion ? "on" : "off"));
            }
            help_marker(tr("With little indirect light, RR cannot accumulate the raw GI while the camera moves (the noise shows).\n"
                           "While you move, the game's filters with history x4 are used (clean in motion), and after staying still\n"
                           "for about 20 frames Full RR comes back. When you start moving the GI history is dropped once so the\n"
                           "game does not drag the raw GI along. Tune the thresholds with the live values: standing still must say \"still\".",
                           "Con poca luz indirecta, RR no puede acumular el GI crudo mientras la c\xC3\xA1mara se mueve (se ve el ruido).\n"
                           "Mientras te mueves se usan los filtros del juego con historial x4 (limpio en movimiento) y al quedarte quieto\n"
                           "unos 20 frames vuelve RR completo. Al empezar a moverte se borra una vez el historial del GI para que el\n"
                           "juego no arrastre el GI crudo. Ajusta los umbrales mirando los valores en vivo: quieto debe decir \"quieto\"."));
            if (motion)
            {
                int mm = rr::cfg::gi_motion_mm.load();
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
                if (ImGui::SliderInt(tr("Movement threshold (mm per frame)", "Umbral de desplazamiento (mm por frame)"), &mm, 1, 200))
                {
                    rr::cfg::gi_motion_mm = mm;
                    rr::cfg::save();
                }
                int mdeg = rr::cfg::gi_motion_mdeg.load();
                float deg = mdeg * 0.001f;
                ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
                if (ImGui::SliderFloat(tr("Turn threshold (degrees per frame)", "Umbral de giro (grados por frame)"), &deg, 0.005f, 1.0f, "%.3f", ImGuiSliderFlags_Logarithmic))
                {
                    rr::cfg::gi_motion_mdeg = int(deg * 1000.0f + 0.5f);
                    rr::cfg::save();
                }
                const rr::gi_motion_snapshot m = rr::gi_motion_get();
                char text[256];
                snprintf(text, sizeof(text), tr("%s: %.1f mm and %.3f\xC2\xB0 in the last frame; %llu starts", "%s: %.1f mm y %.3f\xC2\xB0 en el \xC3\xBAltimo frame; %llu arranques"),
                         m.moving ? tr("moving \xE2\x86\x92 GI history x4", "en movimiento \xE2\x86\x92 GI historial x4") : tr("still \xE2\x86\x92 Full RR", "quieto \xE2\x86\x92 RR completo"),
                         m.move_m * 1000.0f, m.turn_deg, (unsigned long long)m.switches);
                status_line({ m.moving ? level::info : level::ok, text });
            }
            ImGui::Unindent();
        }

        ImGui::Spacing();
        bool hit = rr::cfg::rr_hit_distance.load();
        if (ImGui::Checkbox(tr("Diffuse hit distance for RR (experimental)", "Distancia de impacto difusa para RR (experimental)"), &hit))
        {
            rr::cfg::rr_hit_distance = hit;
            rr::cfg::save();
            rr::cfg::reset_history = true;
            rr::log_info(std::string("overlay: diffuse hit distance guide ") + (hit ? "on" : "off"));
        }
        help_marker(tr("Gives RR the distance at which every indirect light ray hits (DLSSD.DiffuseHitDistance), which the game already\n"
                       "computes: RR then knows whether the light comes from something close or far and can accumulate better, mostly in motion.\n"
                       "It is copied from the game's final GI (after its upscale) and converted to metres: distance = scale x w\xC2\xB2.\n"
                       "Not measured yet: compare with \"Measure\" and \"Measure disocclusion\", and by eye moving slowly in a dark area.",
                       "Le pasa a RR la distancia a la que choca cada rayo de luz indirecta (DLSSD.DiffuseHitDistance), que el juego ya\n"
                       "calcula: as\xC3\xAD RR sabe si la luz viene de algo pegado o lejano y puede acumular mejor, sobre todo en movimiento.\n"
                       "Se copia del GI final del juego (tras su escalado) y se convierte a metros: distancia = escala x w\xC2\xB2.\n"
                       "Sin medir todav\xC3\xAD" "a: compara con \"Medir\" y \"Medir desoclusi\xC3\xB3n\", y a ojo movi\xC3\xA9ndote despacio en una zona oscura."));
        if (hit)
        {
            int scale = rr::cfg::rr_hit_scale.load();
            ImGui::Indent();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
            if (ImGui::SliderInt(tr("Scale (metres for w = 1)", "Escala (metros para w = 1)"), &scale, 1, 50))
            {
                rr::cfg::rr_hit_scale = scale;
                rr::cfg::save();
            }
            ImGui::Unindent();
            const rr::guides_snapshot gs = rr::guides_get_snapshot();
            feedback f { level::pending, tr("waiting for the game's final GI (RTGI upscale pass)", "esperando el GI final del juego (pase de escalado del RTGI)") };
            if (gs.gi_copies > 0 && s.hit_tagged > 0)
                f = { gs.gi_source_frames > 1 ? level::ok : level::pending,
                      tr("applied: ", "aplicado: ") + std::to_string(gs.gi_copies) + tr(" GI copies (texture ", " copias del GI (textura ") + rr::hex(gs.gi_source) + ", " +
                          std::to_string(gs.gi_source_frames) + tr(" frames in a row the same one), ", " frames seguidos la misma), ") + std::to_string(s.hit_tagged) +
                          tr(" RR evaluations with the guide", " evaluaciones de RR con la gu\xC3\xAD" "a") };
            else if (gs.gi_copies > 0)
                f = { level::pending, tr("GI copied (", "GI copiado (") + std::to_string(gs.gi_copies) +
                                          tr("), but RR has not received the guide yet (is RR on?)", "), pero RR a\xC3\xBA" "n no recibi\xC3\xB3 la gu\xC3\xAD" "a (\xC2\xBFRR activo?)") };
            status_line(f);
        }
        if (mode >= 3)
            ImGui::TextColored(kYellow, "%s", tr("Experimental: RR denoises all the GI. Known issues: raw noise for a few frames when moving or when\n"
                                                 "something comes back on screen, and some fireflies at night. If it looks too noisy, use \"Game original\".",
                                                 "Experimental: RR limpia todo el GI. Problemas conocidos: ruido crudo unos frames al moverse o cuando algo\n"
                                                 "vuelve a entrar en pantalla, y algunos destellos de noche. Si se ve demasiado ruido, usa \"Original del juego\"."));

        ImGui::Spacing();
        if (!ImGui::BeginTable("##gi_status", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
            return;
        ImGui::TableSetupColumn("##k", ImGuiTableColumnFlags_WidthStretch, 0.45f);
        ImGui::TableSetupColumn("##v", ImGuiTableColumnFlags_WidthStretch, 0.55f);
        row(tr("Temporal filter (0x4DAF8A48)", "Filtro temporal (0x4DAF8A48)"), g.temporal_found ? &kGreen : &kYellow, "%s",
            g.temporal_found ? tr("found", "encontrado") : tr("not seen yet", "no visto todav\xC3\xAD" "a"));
        row(tr("Spatial filter (0x209AB6A4)", "Filtro espacial (0x209AB6A4)"), g.spatial_found ? &kGreen : &kYellow, "%s",
            g.spatial_found ? tr("found", "encontrado") : tr("not seen yet", "no visto todav\xC3\xAD" "a"));
        row(tr("Variants ready", "Variantes listas"), g.failures > 0 ? &kRed : nullptr, "%d%s", g.twins, g.failures > 0 ? tr("  (with failures, see Log)", "  (con fallos, ver Log)") : "");
        row(tr("Pipeline hook", "Hook de pipelines"), g.hook_installed ? &kGreen : &kYellow, "%s",
            g.hook_installed ? tr("installed", "instalado") : tr("pending (after ~120 frames)", "pendiente (tras ~120 frames)"));
        row(tr("Swaps applied", "Cambios aplicados"), nullptr, "%llu", (unsigned long long)g.swaps);
        if (!g.last_failure.empty())
            row(tr("Last error", "\xC3\x9Altimo error"), &kRed, "%s", g.last_failure.c_str());
        ImGui::EndTable();
        if (!g.temporal_found)
            ImGui::TextDisabled("%s", tr("If it still does not appear in game, a game update may have changed the shader.",
                                         "Si sigue sin aparecer en partida, puede que una actualizaci\xC3\xB3n del juego haya cambiado el shader."));
    }

    void draw_bench()
    {
        const rr::bench_snapshot b = rr::bench_get_snapshot();
        ImGui::TextWrapped("%s", tr("Compares RR with normal DLSS with real numbers instead of judging by eye. With the car and the camera still, "
                                    "it records 8 frames of each variant (after letting RR settle) and measures:",
                                    "Compara RR con el DLSS normal con n\xC3\xBAmeros reales, sin juzgar a ojo. Con el carro y la c\xC3\xA1mara quietos, "
                                    "graba 8 frames de cada variante (tras dejar que RR se estabilice) y mide:"));
        ImGui::BulletText("%s", tr("detail per distance (near, mid, far, very far), separated from noise;", "detalle por distancia (cerca, media, lejos, muy lejos), separado del ruido;"));
        ImGui::BulletText("%s", tr("sky contamination on tree tops and thin edges;", "contaminaci\xC3\xB3n del cielo en copas y bordes finos;"));
        ImGui::BulletText("%s", tr("whether the camera moved (the measurement is then marked as not valid).",
                                   "si la c\xC3\xA1mara se movi\xC3\xB3 (entonces la medici\xC3\xB3n se marca como no v\xC3\xA1lida)."));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("Variants: normal DLSS (reference), RR with your settings, RR with the hit distance guide toggled and a control run at the end. Optional:",
                                     "Variantes: DLSS normal (referencia), RR con tus ajustes, RR con la distancia de impacto al rev\xC3\xA9s y un control al final. Opcionales:"));
        bool neutral = rr::bench_include_neutral.load(), gi = rr::bench_include_gi_original.load();
        bool sharpen_variants = rr::bench_include_sharpen.load();
        bool gi_modes = rr::bench_include_gi_modes.load();
        ImGui::BeginDisabled(b.running);
        if (ImGui::Checkbox(tr("Compare the game's GI: original, history x2 and x4 (boiling on walls)", "Comparar GI del juego: original, historial x2 y x4 (hervor en paredes)"), &gi_modes))
            rr::bench_include_gi_modes = gi_modes;
        if (ImGui::Checkbox(tr("Add RR with neutral guides (is detail lost because of the guides?)",
                               "A\xC3\xB1" "adir RR con gu\xC3\xAD" "as neutras (\xC2\xBF" "el detalle se pierde por las gu\xC3\xAD" "as?)"), &neutral))
            rr::bench_include_neutral = neutral;
        if (ImGui::Checkbox(tr("Add RR with the game's original GI", "A\xC3\xB1" "adir RR con el GI original del juego"), &gi))
            rr::bench_include_gi_original = gi;
        if (ImGui::Checkbox(tr("Add RR with sharpening at 30, 60 and 90 %", "A\xC3\xB1" "adir RR con afinado al 30, 60 y 90 %"), &sharpen_variants))
            rr::bench_include_sharpen = sharpen_variants;
        ImGui::EndDisabled();
        ImGui::Spacing();

        if (!b.running)
        {
            if (ImGui::Button(tr("Measure (stay still for about 20 s)", "Medir (qu\xC3\xA9" "date quieto unos 20 s)")))
                rr::bench_start(0);
            if (ImGui::Button(tr("Measure car reflections", "Medir reflejos del carro")))
                rr::bench_start(1);
            help_marker(tr("Stop the car with the bodywork reflecting the surroundings. Compares metalness, specular fade and the GI mode\n"
                           "(Full RR against the original) and measures noise and detail only on the car (everything closer than 8 m).",
                           "Para el carro con la carrocer\xC3\xAD" "a reflejando el entorno. Compara metalness, especular atenuado y el modo de GI\n"
                           "(RR completo frente al original) y mide el ruido y el detalle solo en el carro (todo lo que est\xC3\xA1 a menos de 8 m)."));
            if (ImGui::Button(tr("Measure disocclusion (noise when moving)", "Medir desoclusi\xC3\xB3n (ruido al moverse)")))
                rr::bench_start(3);
            help_marker(tr("What you see when moving or when something leaves the screen and comes back: pixels without history.\n"
                           "With the camera still, it drops RR's history and the game's GI history at the same time (the whole screen becomes\n"
                           "\"just revealed\") and records the next 8 frames. Compares original GI, Full RR and Full RR with\n"
                           "disocclusion assist (8 frames) against their own clean image: \"Disocclusion\" table of the report.\n"
                           "Stay still looking at a scene with walls and shadows, with no cars crossing.",
                           "Lo que se ve al moverse o cuando algo sale de la pantalla y vuelve: p\xC3\xADxeles sin historial.\n"
                           "Con la c\xC3\xA1mara quieta, borra a la vez el historial de RR y el del GI del juego (toda la pantalla queda como\n"
                           "reci\xC3\xA9n aparecida) y graba los 8 frames siguientes. Compara GI original, RR completo y RR completo con\n"
                           "desoclusi\xC3\xB3n asistida (8 frames) frente a su imagen ya limpia: tabla \"Desoclusi\xC3\xB3n\" del informe.\n"
                           "Qu\xC3\xA9" "date quieto mirando una escena con paredes y sombras, sin carros cruzando."));
            if (ImGui::Button(tr("Measure night fireflies (Full RR)", "Medir destellos de noche (RR completo)")))
                rr::bench_start(2);
            help_marker(tr("At night, in front of a wall lit indirectly where the dots appear. Measures Full RR without clamp, with the isolated\n"
                           "sample clamp at k = 4, 2 and 1, and with k = 2 + v2 medium. Look above all at the noise maps and the brightness in the report.",
                           "De noche, frente a una pared con luz indirecta donde aparezcan los puntos. Mide RR completo sin recorte, con recorte de muestra\n"
                           "aislada k = 4, 2 y 1, y con k = 2 + recorte v2 media. Mira sobre todo los mapas de ruido y el brillo del informe."));
            help_marker(tr("Stop the car and do not touch the camera or the controller until it finishes. The add-on switches between variants\n"
                           "by itself and restores your settings at the end. Best by day, with far fences or railings and trees against the sky in view.",
                           "Para el carro, no toques la c\xC3\xA1mara ni el mando hasta que termine. El addon cambia solo entre variantes\n"
                           "y al final restaura tus ajustes. Mejor de d\xC3\xAD" "a, con vallas o barandas lejanas y \xC3\xA1rboles contra el cielo a la vista."));
        }
        else if (ImGui::Button(tr("Cancel measurement", "Cancelar medici\xC3\xB3n")))
            rr::bench_abort();

        ImGui::Spacing();
        const ImVec4 *color = !b.error.empty() ? &kRed : b.running ? &kYellow : (b.summary.empty() ? &kGrey : &kGreen);
        const char *tag = !b.error.empty() ? "[ERROR]" : b.running ? "[...]" : (b.summary.empty() ? "[i]" : "[OK]");
        ImGui::TextColored(*color, "%s %s", tag, b.error.empty() ? b.state.c_str() : b.error.c_str());
        if (b.running)
        {
            char overlay_text[64];
            snprintf(overlay_text, sizeof(overlay_text), "%.0f %%", b.progress * 100.0f);
            ImGui::ProgressBar(b.progress, ImVec2(-1.0f, 0.0f), overlay_text);
            ImGui::TextColored(kYellow, "%s", tr("Do not move the car or the camera.", "No muevas el carro ni la c\xC3\xA1mara."));
        }
        if (!b.summary.empty())
        {
            ImGui::SeparatorText(tr("Result (ratios against normal DLSS: 1.00 = same)", "Resultado (ratios contra DLSS normal: 1,00 = igual)"));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(b.summary.c_str());
            ImGui::PopTextWrapPos();
        }
        if (!b.folder.empty())
            ImGui::TextDisabled(tr("Full report: %s\\report.txt", "Informe completo: %s\\report.txt"), b.folder.c_str());
    }

    std::string g_export_status;   // last "Export game shaders" result (present thread only)

    void draw_developer()
    {
        const std::string root = rr::capture_root().string();
        ImGui::TextWrapped(tr("Tools for modders. Files go to %s (change it with CaptureDir in ReShade.ini, section [RR_Forza]).",
                              "Herramientas para modders. Los archivos van a %s (se cambia con CaptureDir en ReShade.ini, secci\xC3\xB3n [RR_Forza])."),
                           root.c_str());
        ImGui::Spacing();

        bool dev_keys = rr::cfg::dev_hotkeys.load();
        if (ImGui::Checkbox(tr("Developer hotkeys (F7, F10)", "Teclas de desarrollo (F7, F10)"), &dev_keys))
        {
            rr::cfg::dev_hotkeys = dev_keys;
            rr::cfg::save();
        }
        help_marker(tr("Off by default so that a stray F10 does not write a capture of up to 1 GB.\n"
                       "F7 = metalness from the albedo alpha; F10 / Shift+F10 / Ctrl+Shift+F10 = the three captures below.\n"
                       "F11 (Ray Reconstruction on/off) always works.",
                       "Apagadas por defecto para que un F10 sin querer no escriba una captura de hasta 1 GB.\n"
                       "F7 = metalness desde el alfa del albedo; F10 / Shift+F10 / Ctrl+Shift+F10 = las tres capturas de abajo.\n"
                       "F11 (Ray Reconstruction s\xC3\xAD/no) funciona siempre."));

        ImGui::SeparatorText(tr("Game shaders for building", "Shaders del juego para compilar"));
        if (ImGui::Button(tr("Export game shaders", "Exportar shaders del juego")))
            g_export_status = rr::export_game_shaders();
        help_marker(tr("Writes the two RTGI filter shaders of YOUR copy of the game (RTGI_TemporalFilter 0x4DAF8A48 and\n"
                       "RTGI_SpatialFilter_Disk 0x209AB6A4) to <capture folder>\\game-shaders. Copy both files into the\n"
                       "game-shaders folder of the source code and rebuild: the GI modes are patched from them.\n"
                       "They are not in the repository because they are the game's code.",
                       "Escribe los dos shaders de los filtros del RTGI de TU copia del juego (RTGI_TemporalFilter 0x4DAF8A48 y\n"
                       "RTGI_SpatialFilter_Disk 0x209AB6A4) en <carpeta de capturas>\\game-shaders. Copia ambos archivos a la\n"
                       "carpeta game-shaders del c\xC3\xB3" "digo fuente y vuelve a compilar: los modos de GI se parchean a partir de ellos.\n"
                       "No est\xC3\xA1n en el repositorio porque son c\xC3\xB3" "digo del juego."));
        if (!g_export_status.empty())
        {
            // "2/2 ..." = done; "0/2" or "1/2 ..." = the game has not created a filter yet; anything else = file error.
            const level lv = g_export_status.rfind("2/2", 0) == 0 ? level::ok : g_export_status.find("/2 ") != std::string::npos ? level::pending : level::error;
            status_line({ lv, g_export_status });
        }

        ImGui::SeparatorText(tr("Frame capture", "Captura de frame"));
        ImGui::TextWrapped("%s", tr("Dumps one frame to analyse it (textures, guides, passes and shaders).",
                                    "Vuelca un frame para analizarlo (texturas, gu\xC3\xAD" "as, pases y shaders)."));
        if (ImGui::Button(tr("Capture frame (F10)", "Capturar frame (F10)")))
            rr::request_capture(1);
        help_marker(tr("No state changes in the game: the safest.", "Sin cambios de estado en el juego: lo m\xC3\xA1s seguro."));
        if (ImGui::Button(tr("Capture with state changes (Shift+F10)", "Capturar con cambios de estado (Shift+F10)")))
            rr::request_capture(2);
        help_marker(tr("Also copies textures that need their state changed with round-trip barriers.",
                       "Tambi\xC3\xA9n copia texturas que exigen cambiar su estado con barreras de ida y vuelta."));
        if (ImGui::Button(tr("RT-focused capture (Ctrl+Shift+F10)", "Captura enfocada en RT (Ctrl+Shift+F10)")))
            rr::request_capture(3);
        help_marker(tr("Only the passes inside RenderRTBufferEffects, with a dump of their shaders.",
                       "Solo los pases dentro de RenderRTBufferEffects, con volcado de sus shaders."));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("State:", "Estado:"));
        ImGui::SameLine();
        ImGui::TextWrapped("%s", rr::capture_state_text().c_str());
    }

    void draw_help()
    {
        ImGui::SeparatorText(tr("Keys", "Teclas"));
        ImGui::BulletText("%s", tr("F11: Ray Reconstruction / normal DLSS", "F11: Ray Reconstruction / DLSS normal"));
        ImGui::BulletText("%s", tr("F7: metalness from the alpha (experimental, developer hotkeys)", "F7: metalness desde el alfa (experimental, teclas de desarrollo)"));
        ImGui::BulletText("%s", tr("F10, Shift+F10, Ctrl+Shift+F10: captures (developer hotkeys)", "F10, Shift+F10, Ctrl+Shift+F10: capturas (teclas de desarrollo)"));
        ImGui::SeparatorText(tr("Files", "Archivos"));
        ImGui::BulletText("%s", tr("Settings: ReShade.ini, section [RR_Forza]", "Ajustes: ReShade.ini, secci\xC3\xB3n [RR_Forza]"));
        ImGui::BulletText(tr("Captures and measurements: %s", "Capturas y mediciones: %s"), rr::capture_root().string().c_str());
        ImGui::BulletText(tr("Project, source code and bug reports: %s", "Proyecto, c\xC3\xB3" "digo y reporte de errores: %s"), RR_FORZA_REPO);
        ImGui::SeparatorText(tr("Known limits", "L\xC3\xADmites conocidos"));
        ImGui::TextWrapped("%s", tr("Full RR: raw noise for a few frames in motion and on disocclusion, some fireflies at night, noisy mirror-like "
                                    "reflections (RR gets no specular motion vectors yet) and tree tops that can burn against the sky. "
                                    "Forza's reflections have little stochastic noise, so RR adds little there.",
                                    "RR completo: ruido crudo unos frames en movimiento y en desoclusi\xC3\xB3n, algunos destellos de noche, reflejos tipo "
                                    "espejo con ruido (RR a\xC3\xBA" "n no recibe vectores de movimiento especulares) y copas de \xC3\xA1rboles que pueden quemarse "
                                    "contra el cielo. Los reflejos de Forza tienen poco ruido estoc\xC3\xA1stico, as\xC3\xAD que ah\xC3\xAD RR aporta poco."));
        ImGui::Spacing();
        ImGui::TextDisabled("%s", tr("Safety: the game, its exe and its DLLs are not modified; everything happens in memory.",
                                     "Seguridad: el juego, su exe y sus DLL no se modifican; todo ocurre en memoria."));
    }

    void draw_overlay(reshade::api::effect_runtime *)
    {
        const rr::rr_snapshot s = rr::rr_get_snapshot();
        const rr::guides_snapshot g = rr::guides_get_snapshot();
        g_rates.update(s, g);

        ImGui::Text("%s  " RR_FORZA_VERSION, tr("Ray Reconstruction for Forza Horizon 6", "Ray Reconstruction para Forza Horizon 6"));
        ImGui::Separator();
        draw_status(s, g);
        ImGui::Spacing();

        // "###id": the tab keeps its identity (and stays selected) when the language changes.
        if (!ImGui::BeginTabBar("##rr_tabs", 0))
            return;
        if (ImGui::BeginTabItem("General###general", nullptr, 0))
        {
            draw_general(s);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Guides###guides", "Gu\xC3\xAD" "as###guides"), nullptr, 0))
        {
            draw_guides(s, g);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Game GI###gi", "GI del juego###gi"), nullptr, 0))
        {
            draw_gi(s);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Measurement###bench", "Medici\xC3\xB3n###bench"), nullptr, 0))
        {
            draw_bench();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Developer###dev", "Desarrollo###dev"), nullptr, 0))
        {
            draw_developer();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Help###help", "Ayuda###help"), nullptr, 0))
        {
            draw_help();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

namespace
{
    // Hotkey confirmation, drawn in the top-left corner for 4 s even with the overlay closed. The
    // second line is live: it turns from "[...]" to "[OK]" once the change is confirmed on the GPU.
    std::mutex g_notice_mutex;
    rr::notice g_notice_what = rr::notice::rr_toggle;
    uint64_t g_notice_tick = 0;
    constexpr uint64_t kNoticeMs = 4000;

    void on_reshade_overlay(reshade::api::effect_runtime *)
    {
        rr::notice what;
        uint64_t tick;
        {
            std::lock_guard lock(g_notice_mutex);
            what = g_notice_what;
            tick = g_notice_tick;
        }
        if (!fresh(tick, kNoticeMs))
            return;

        const rr::rr_snapshot s = rr::rr_get_snapshot();
        std::string title;
        feedback f { level::info, "" };
        switch (what)
        {
        case rr::notice::rr_toggle:
            title = std::string("F11  Ray Reconstruction: ") + (rr::cfg::rr_enabled.load() ? tr("ON", "ENCENDIDO") : tr("OFF", "APAGADO"));
            f = fb_rr(s);
            break;
        case rr::notice::metal_toggle:
        {
            const rr::guides_snapshot g = rr::guides_get_snapshot();
            title = std::string(tr("F7  Metalness from the alpha: ", "F7  Metalness desde el alfa: ")) + (rr::cfg::metal_alpha.load() ? tr("ON", "ENCENDIDO") : tr("OFF", "APAGADO"));
            f = fb_guide(s, g, g.applied_metal != 0, rr::cfg::metal_alpha.load(),
                         tr("subtle effect: only reflections on metal parts", "efecto sutil: solo reflejos de partes met\xC3\xA1licas"));
            break;
        }
        case rr::notice::capture:
            title = tr("F10  Frame capture", "F10  Captura de frame");
            f = { level::info, rr::capture_state_text() };
            break;
        }

        ImGui::SetNextWindowPos(ImVec2(16.0f, 16.0f), ImGuiCond_Always, ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.80f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                                       ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings;
        if (ImGui::Begin("##rr_forza_notice", nullptr, flags))
        {
            ImGui::TextUnformatted(title.c_str());
            const ImVec4 *color = f.lv == level::ok ? &kGreen : f.lv == level::error ? &kRed : f.lv == level::pending ? &kYellow : &kGrey;
            const char *tag = f.lv == level::ok ? "[OK]" : f.lv == level::error ? "[ERROR]" : f.lv == level::pending ? "[...]" : "[i]";
            ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
            ImGui::TextColored(*color, "%s %s", tag, f.text.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::End();
    }
}

namespace rr
{
    void notify(notice what)
    {
        std::lock_guard lock(g_notice_mutex);
        g_notice_what = what;
        g_notice_tick = GetTickCount64();
    }

    void register_overlay_ui()
    {
        reshade::register_overlay("Ray Reconstruction", draw_overlay);
        reshade::register_event<reshade::addon_event::reshade_overlay>(on_reshade_overlay);
    }

    void unregister_overlay_ui()
    {
        reshade::unregister_event<reshade::addon_event::reshade_overlay>(on_reshade_overlay);
        reshade::unregister_overlay("Ray Reconstruction", draw_overlay);
    }
}
