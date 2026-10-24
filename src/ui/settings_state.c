/* Settings focus and commit state. No rendering, dialogs, or file writes. */
#include "ui/settings.h"
#include "ui/settings_layout.h"
#include "vault/crypto.h"
#include <string.h>
#include <stdio.h>

void settings_clear_focus(SettingsPanel *sp) {
    if (!sp) return;
    sp->rebinding = false;
    sp->reset_armed = sp->vo_model_del_armed = sp->tr_model_del_armed = false;
    sp->translate_lang_picker = SETTINGS_LANG_PICKER_CLOSED;
    sp->translate_model_picker_open = false;
    sp->voice_lang_picker_open = sp->voice_mic_picker_open = false;
    sp->editing_api_key = sp->editing_api_url = sp->editing_api_model = false;
    crypto_secure_zero(sp->api_key_buf, sizeof sp->api_key_buf);
    crypto_secure_zero(sp->api_url_buf, sizeof sp->api_url_buf);
    crypto_secure_zero(sp->api_model_buf, sizeof sp->api_model_buf);
    crypto_secure_zero(sp->vault_old_pw, sizeof sp->vault_old_pw);
    crypto_secure_zero(sp->vault_new_pw, sizeof sp->vault_new_pw);
    crypto_secure_zero(sp->vault_new_pw2, sizeof sp->vault_new_pw2);
    sp->vault_editing_old_pw = sp->vault_editing_new_pw = sp->vault_editing_new_pw2 = false;
    sp->browser_rule_editing = false;
    sp->browser_rule_index = -1;
    sp->browser_rule_buf[0] = sp->browser_rule_status[0] = '\0';
    sp->browser_rule_status_error = false;
    sp->drag_slider = SETTINGS_SLIDER_NONE;
    sp->sb_dragging = false;
    sp->vo_model_dl_w = sp->vo_model_cancel_w = sp->vo_model_pick_w = sp->vo_model_del_w = 0;
    sp->tr_model_dl_w = sp->tr_model_cancel_w = sp->tr_model_pick_w = sp->tr_model_del_w = 0;
}

static bool commit_text(char *dest, usize cap, const char *value) {
    if (strcmp(dest, value) == 0) return false;
    snprintf(dest, cap, "%s", value);
    return true;
}

bool settings_commit_api_editors(SettingsPanel *sp) {
    if (!sp || !sp->config) return false;
    if (sp->api_edit_tab != SETTINGS_TAB_TRANSLATE && sp->api_edit_tab != SETTINGS_TAB_VOICE)
        return false;
    bool changed = false, voice = sp->api_edit_tab == SETTINGS_TAB_VOICE;
    TranslateConfig *tc = &sp->config->translate;
    VoiceConfig *vc = &sp->config->voice;
    if (sp->editing_api_key) {
        changed |= voice ? commit_text(vc->api_key, sizeof vc->api_key, sp->api_key_buf)
                         : commit_text(tc->api_key, sizeof tc->api_key, sp->api_key_buf);
        crypto_secure_zero(sp->api_key_buf, sizeof sp->api_key_buf);
    }
    if (sp->editing_api_url)
        changed |= voice ? commit_text(vc->api_base_url, sizeof vc->api_base_url, sp->api_url_buf)
                         : commit_text(tc->api_base_url, sizeof tc->api_base_url, sp->api_url_buf);
    if (sp->editing_api_model)
        changed |= voice ? commit_text(vc->api_model, sizeof vc->api_model, sp->api_model_buf)
                         : commit_text(tc->api_model, sizeof tc->api_model, sp->api_model_buf);
    sp->editing_api_key = sp->editing_api_url = sp->editing_api_model = false;
    sp->config_dirty |= changed;
    return changed;
}

bool settings_capture_binding(SettingsPanel *sp, u32 key, u32 mods) {
    if (!sp || !sp->rebinding || sp->active_tab != SETTINGS_TAB_KEYS) return false;
    if (key == KEY_ESCAPE) { sp->rebinding = false; return true; }
    if (key == KEY_UNKNOWN || (key >= KEY_LSHIFT && key <= KEY_RSUPER)) return true;
    if (sp->keybinds) {
        if (key == KEY_DELETE || key == KEY_BACKSPACE) {
            sp->keybindings_dirty |= keybind_get(sp->keybinds, (Action)sp->rebind_action) != NULL;
            keybind_remove(sp->keybinds, (Action)sp->rebind_action);
        } else {
            keybind_set(sp->keybinds, (Action)sp->rebind_action, (KeyCode)key, mods);
            sp->keybindings_dirty = true;
        }
    }
    sp->rebinding = false;
    return true;
}
