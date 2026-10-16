#include "core/JoystickWizard.h"
#include "lua/InputMap.h" // kRawJoyMaxButtons/kRawJoyMaxHats/kRawJoyMaxAxes

#include <cstdio>

namespace dirk {

const std::vector<JoystickWizard::PromptDef>& JoystickWizard::prompts() {
    // switchId values match framework.kimmy's SWITCH_* enum exactly (see
    // InputMap.cpp's kBindings, which this list is a curated subset of).
    //
    // 2026-09-24 (Cyril, after trying the English-labelled 19-entry version
    // this replaced): "On propose trop de trucs. Ce que je veux c'est ne
    // proposer que les interactions de Kimmy en les nommant pareil." He
    // then gave this exact list of labels himself. Two entries from the
    // previous list are gone entirely (not just relabelled) because he
    // simply didn't include them: SWITCH_COIN2 (10, second coin slot) and
    // SWITCH_CONSOLE (20, menu back/cancel) -- both real, both still fully
    // supported by controls.cfg/switchMap for anyone who binds them by
    // hand, just no longer offered by this wizard. Everything else
    // moves.kimmy actually dispatches on (confirmed by reading its
    // onInputPressed/onInputReleased directly, not assumed) is still
    // here. Still plain ASCII throughout (see the top-of-file 2026-09-23
    // note on SDL_RenderDebugText).
    //
    // Order is ALSO Cyril's own, verbatim (2026-09-24, same day, right
    // after seeing this list run for the first time: "c'est genant si on
    // les met dans l'ordre que j'ai indique ?" -- no, not at all: each
    // entry captures independently by its own switchId, never by its
    // position in this vector, so reordering this list is purely
    // cosmetic and changes nothing about how a saved profile behaves).
    // His original order was: Haut, Bas, Gauche, Droite, Start, Coin,
    // Bouton 1, Bouton 2, Bouton 3, Pause Game, Option Menu, Save Menu,
    // Quit Menu, 2P start, Save slot 1, Save slot 2, Save slot 3.
    //
    // 2026-09-24, later the same day -- real mislabeling caught by Cyril
    // ("l'action QUIT me semble liee a Input_quit alors qu'en fait c'est
    // INPUT_CONSOLE je crois"), verified against InputMap.cpp's kBindings
    // and confirmed correct: switch 18 (INPUT_QUIT, alias QUIT_PROGRAM)
    // is the whole-program exit ("if user pressed ESC then quit program",
    // real usage confirmed in drugwars/johnnyrock/lastbountyhunter/
    // crimepatrol/spacepirates/maddog2/maddog) -- NOT what "Quit Menu"
    // means. Switch 20 (INPUT_CONSOLE, alias EXIT_MENU -- literally
    // "exit menu") is the one that actually matches "Quit Menu". Fixed:
    // this entry now points at switch 20. Every one of the other 16
    // entries was re-checked against kBindings' alias column at the same
    // time (his suggestion, "pour la clarte") and all 16 already matched
    // their alias correctly -- this was the only mismatch.
    //
    // 2026-09-25: labels translated to English (Haut->Up, Bas->Down,
    // Gauche->Left, Droite->Right, Bouton N->Button N). An earlier version
    // of this comment argued these should stay exactly as Cyril first
    // typed them, mixed French/English, since "naming them the same"
    // meant his words verbatim -- but he then repeated, explicitly and
    // more than once, that ALL of this screen's text must be in English,
    // with no exception carved out for these labels. That instruction
    // supersedes the earlier one; this is what's applied now.
    static const std::vector<PromptDef> kPrompts = {
        {0,  "Up",           "GO_UP"},
        {2,  "Down",         "GO_DOWN"},
        {1,  "Left",         "GO_LEFT"},
        {3,  "Right",        "GO_RIGHT"},
        {4,  "Start",        "START_1P"},
        {9,  "Coin",         "COIN_1P"},
        {6,  "Button 1",     "BUTTON_1"},
        {7,  "Button 2",     "BUTTON_2"},
        {8,  "Button 3",     "BUTTON_3"},
        {19, "Pause Game",   "PAUSE_GAME"},
        {14, "Option Menu",  "OPTION_MENU"},
        {15, "Save Menu",    "SAVE_MENU"},
        {20, "Quit Menu",    "EXIT_MENU"},  // was switch 18 (QUIT_PROGRAM) -- fixed, see comment above
        {5,  "2P start",     "START_2P"},
        {11, "Save slot 1",  "SAVE_1"},
        {12, "Save slot 2",  "SAVE_2"},
        {13, "Save slot 3",  "SAVE_3"},
    };
    return kPrompts;
}

JoystickWizard::JoystickWizard(std::string sharedEngineRoot, std::string gameFolder,
                                JoystickProfileStore& gameProfiles, JoystickProfileStore& generalProfiles)
    : sharedEngineRoot_(std::move(sharedEngineRoot)),
      gameFolder_(std::move(gameFolder)),
      gameProfilePath_(gameFolder_.empty() ? std::string() : JoystickProfileStore::defaultPathForGame(gameFolder_)),
      generalProfilePath_(JoystickProfileStore::defaultPath(sharedEngineRoot_)),
      gameProfiles_(gameProfiles),
      generalProfiles_(generalProfiles) {
    // Same font the loading screen already uses (main.cpp), resolved the
    // same way (resolveSharedAsset(), JoystickProfile.h) -- kept open for
    // the engine's whole lifetime rather than per-wizard-session, same
    // reasoning as any other loaded-once asset; a missing font file just
    // means the wizard draws no text (still functions, just invisible --
    // matches the loading screen's own "optional, cosmetic" treatment of
    // this same file).
    const std::string fontPath = resolveSharedAsset(sharedEngineRoot_, "dirk_loading_font.ttf");
    font_ = TTF_OpenFont(fontPath.c_str(), 26);
    if (!font_) {
        std::fprintf(stderr, "[dirk] warning: joystick wizard couldn't open dirk_loading_font.ttf (checked %s"
                     "/KimmyScript/Cfg/ and %s directly) -- falling back to SDL's built-in debug text "
                     "(smaller, but always readable)\n",
                     sharedEngineRoot_.c_str(), sharedEngineRoot_.c_str());
    }
}

JoystickWizard::~JoystickWizard() {
    releaseFont(); // no-op if main.cpp already did it before TTF_Quit() -- see releaseFont()'s comment
}

void JoystickWizard::releaseFont() {
    if (font_) {
        TTF_CloseFont(font_);
        font_ = nullptr;
    }
}

bool JoystickWizard::isUsingDevice(SDL_JoystickID id) const {
    return step_ == Step::Capture && captureInstanceId_ == id;
}

int JoystickWizard::findDeviceIndex(SDL_JoystickID id) const {
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i].instanceId == id) return static_cast<int>(i);
    }
    return -1;
}

void JoystickWizard::open(lua_State* L, const std::vector<DeviceInfo>& devices) {
    devices_ = devices;
    selectedDeviceIndex_ = 0;
    deleteCandidateIndex_ = -1;
    selectedScopeIndex_ = 0;
    // New first screen (see the top-of-file 2026-09-24 comment): ask
    // "this game or general" before anything else -- EXCEPT when there's
    // no real choice to make: no per-game folder to offer "this game"
    // against (gameFolder_ empty, the legacy bare-video launch mode), or
    // nothing connected to configure either way (devices_ empty, in which
    // case PickDevice's own auto-close timeout is the right next step,
    // not a scope question about nothing). Both cases fall straight
    // through to the exact old behavior: general/shared store, no extra
    // screen.
    if (gameFolder_.empty() || devices_.empty()) {
        beginWithScope(L, /*perGame=*/false);
    } else {
        pendingLuaState_ = L;
        step_ = Step::PickScope;
    }
    std::fprintf(stderr, "[dirk] joystick wizard: ouvert (%zu joystick(s) non reconnu(s) detecte(s))\n", devices_.size());
}

void JoystickWizard::beginWithScope(lua_State* L, bool perGame) {
    scopeIsPerGame_ = perGame;
    activeProfiles_ = perGame ? &gameProfiles_ : &generalProfiles_;
    activePath_ = perGame ? gameProfilePath_ : generalProfilePath_;
    // Refresh from disk every time the wizard opens, in case the file was
    // hand-edited since the engine started -- same defensive reasoning as
    // JoystickProfileStore::load()'s own doc comment.
    activeProfiles_->load(L, activePath_);
    step_ = Step::PickDevice;
    if (devices_.empty()) {
        // See the 2026-09-23 field report at the top of JoystickWizard.h:
        // with nothing to configure, this screen auto-closes on its own
        // rather than waiting indefinitely on a key the player has no
        // guaranteed way to discover.
        deadlineTicks_ = SDL_GetTicks() + kEmptyListTimeoutMs;
    }
    std::fprintf(stderr, "[dirk] joystick wizard: portee = %s (%s)\n",
                 perGame ? "ce jeu" : "generale", activePath_.c_str());
}

void JoystickWizard::cancel() {
    if (step_ != Step::Inactive) {
        std::fprintf(stderr, "[dirk] joystick wizard: annule, rien n'a ete enregistre\n");
    }
    step_ = Step::Inactive;
    deleteCandidateIndex_ = -1;
}

void JoystickWizard::clearPromptBinding(int switchId, bool axesOnly, bool buttonsOnly) {
    if (!axesOnly) {
        for (auto it = binding_.buttons.begin(); it != binding_.buttons.end();) {
            if (it->second == switchId) it = binding_.buttons.erase(it);
            else ++it;
        }
    }
    if (!buttonsOnly) {
        for (auto& entry : binding_.axes) {
            if (entry.second.neg == switchId) entry.second.neg = -1;
            if (entry.second.pos == switchId) entry.second.pos = -1;
        }
        for (auto& entry : binding_.hats) {
            if (entry.second.up == switchId) entry.second.up = -1;
            if (entry.second.down == switchId) entry.second.down = -1;
            if (entry.second.left == switchId) entry.second.left = -1;
            if (entry.second.right == switchId) entry.second.right = -1;
        }
    }
}

bool JoystickWizard::hasButtonBinding(int switchId) const {
    for (const auto& entry : binding_.buttons) {
        if (entry.second == switchId) return true;
    }
    return false;
}

bool JoystickWizard::hasAxisOrHatBinding(int switchId) const {
    for (const auto& entry : binding_.axes) {
        if (entry.second.neg == switchId || entry.second.pos == switchId) return true;
    }
    for (const auto& entry : binding_.hats) {
        if (entry.second.up == switchId || entry.second.down == switchId
            || entry.second.left == switchId || entry.second.right == switchId) return true;
    }
    return false;
}

void JoystickWizard::armDeadline() {
    deadlineTicks_ = SDL_GetTicks() + kPromptTimeoutMs;
}

void JoystickWizard::startCapture(const DeviceInfo& device) {
    captureInstanceId_ = device.instanceId;
    captureKind_ = device.kind;
    for (bool& b : captureAxisNegPressed_) b = false;
    for (bool& b : captureAxisPosPressed_) b = false;
    binding_ = JoystickBinding{};
    binding_.guid = device.guid;
    binding_.name = device.name;
    promptIndex_ = 0;
    secondStickOffered_ = false;
    secondStickActive_ = false;
    secondStickAccepted_ = false;
    secondStickFilter_ = SecondStickFilter::None;
    step_ = Step::Capture;
    armDeadline();
    std::fprintf(stderr, "[dirk] joystick wizard: configuration de \"%s\" (GUID %s, %s)\n",
                 device.name.c_str(), device.guid.c_str(),
                 device.kind == DeviceInfo::Kind::Gamepad ? "manette reconnue" : "joystick brut");
}

void JoystickWizard::advancePrompt() {
    ++promptIndex_;
    // Right after the 4 directional prompts (indices 0-3: Haut/Bas/
    // Gauche/Droite) finish for a recognized gamepad -- offer the
    // second-stick pass, but only once per run (see Step::
    // ConfirmSecondStick's comment in the header).
    if (promptIndex_ == 4 && captureKind_ == DeviceInfo::Kind::Gamepad) {
        if (!secondStickOffered_) {
            secondStickOffered_ = true;
            step_ = Step::ConfirmSecondStick;
            return;
        }
        // Reached again after the second pass itself finished walking
        // 0-3 a second time -- back to ordinary single-capture prompts
        // from here on (Start, Coin, ...).
        secondStickActive_ = false;
    }
    if (promptIndex_ >= prompts().size()) {
        step_ = Step::Confirm;
    } else {
        armDeadline();
    }
}

// See the 2026-09-25 field report at the header declaration of these two.
int JoystickWizard::totalPromptCount() const {
    return static_cast<int>(prompts().size()) + (secondStickAccepted_ ? 4 : 0);
}

int JoystickWizard::currentPromptNumber() const {
    int n = static_cast<int>(promptIndex_) + 1;
    if (secondStickActive_) {
        // The second pass itself: walking prompts 0..3 again, but these
        // are really steps 5..8 once the first pass's 4 are counted.
        n += 4;
    } else if (secondStickAccepted_ && promptIndex_ >= 4) {
        // Tail after both passes (Start, Coin, ...): promptIndex_ keeps
        // counting 4..16 from prompts(), offset by the second pass's 4.
        n += 4;
    }
    return n;
}

void JoystickWizard::goBackPrompt() {
    if (promptIndex_ == 0) return; // already at the first prompt -- nothing earlier to redo
    --promptIndex_;
    if (secondStickActive_) {
        // Only clear what THIS second pass is capturing, so undoing it
        // can't also wipe out the first pass's D-pad (or stick) mapping
        // for the same direction.
        clearPromptBinding(prompts()[promptIndex_].switchId,
                            secondStickFilter_ == SecondStickFilter::AxisOnly,
                            secondStickFilter_ == SecondStickFilter::ButtonOnly);
    } else {
        clearPromptBinding(prompts()[promptIndex_].switchId);
    }
    armDeadline();
}

void JoystickWizard::finishSave() {
    activeProfiles_->upsert(binding_);
    bool ok = activeProfiles_->save(activePath_);
    std::fprintf(stderr, "[dirk] joystick wizard: profil %s pour \"%s\" (GUID %s, portee %s) -> %s\n",
                 ok ? "enregistre" : "ECHEC D'ECRITURE", binding_.name.c_str(), binding_.guid.c_str(),
                 scopeIsPerGame_ ? "ce jeu" : "generale", activePath_.c_str());
    step_ = Step::Inactive;
}

bool JoystickWizard::handleEvent(const SDL_Event& ev) {
    switch (ev.type) {
        case SDL_EVENT_KEY_DOWN:
            if (!ev.key.repeat) return handleKeyDown(ev.key.scancode);
            return true; // swallow held-key repeats too -- never let them leak to gameplay while configuring
        case SDL_EVENT_KEY_UP:
            return true;
        case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            return handleJoyButton(ev.jbutton.which, ev.jbutton.button, true);
        case SDL_EVENT_JOYSTICK_BUTTON_UP:
            return handleJoyButton(ev.jbutton.which, ev.jbutton.button, false);
        case SDL_EVENT_JOYSTICK_HAT_MOTION:
            return handleJoyHat(ev.jhat.which, ev.jhat.hat, ev.jhat.value);
        case SDL_EVENT_JOYSTICK_AXIS_MOTION:
            return handleJoyAxis(ev.jaxis.which, ev.jaxis.axis, ev.jaxis.value);
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            return handleGamepadButton(ev.gbutton.which, ev.gbutton.button, true);
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            return handleGamepadButton(ev.gbutton.which, ev.gbutton.button, false);
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            return handleGamepadAxis(ev.gaxis.which, ev.gaxis.axis, ev.gaxis.value);
        default:
            return false; // QUIT, WINDOW_*, JOYSTICK_ADDED/REMOVED, MOUSE_*, ... -- handled normally by the caller
    }
}

void JoystickWizard::handleGameSwitch(int switchId, bool pressed) {
    if (!pressed || !takesGameControls()) return;
    SDL_Scancode sc = SDL_SCANCODE_UNKNOWN;
    switch (switchId) {
        case 0: sc = SDL_SCANCODE_UP; break;       // SWITCH_UP
        case 2: sc = SDL_SCANCODE_DOWN; break;     // SWITCH_DOWN
        // Not SWITCH_START1 (2026-10-03, R36S): the "other input too?"
        // question comes right before the Start prompt, so a player
        // pressing Start for that prompt answered "yes" instead, and the
        // stick-only second pass then ignored every button (Start "no
        // longer recognised"). Only action 1 confirms.
        case 6: sc = SDL_SCANCODE_RETURN; break;   // SWITCH_BUTTON1
        case 7:                                     // SWITCH_BUTTON2
        case 20: sc = SDL_SCANCODE_ESCAPE; break;  // SWITCH_CONSOLE (exit menu)
        default: return;
    }
    handleKeyDown(sc);
}

bool JoystickWizard::handleKeyDown(SDL_Scancode sc) {
    // 2026-09-24 field report (Cyril): "F9 met la video en pause mais si on
    // rappuie sur F9 (au lieu de escape) la video reste figee". Root cause,
    // confirmed by reading main.cpp's own dispatch loop, not guessed: F9's
    // own handler there only OPENS the wizard, explicitly guarded on
    // !wizard.active() -- so once open, EVERY keydown (F9 included) is
    // instead routed straight into this function (see handleEvent() and
    // main.cpp's `if (wizard.active()) { wizard.handleEvent(ev); ... }`
    // ahead of its own switch). Before this fix, F9 matched no case at any
    // step below and simply did nothing -- silently swallowed, wizard
    // still open, disc still paused -- which looks, to the player, exactly
    // like a hang (no visible difference from one). Cyril's own mental
    // model is a toggle: F9 opens it, F9 closes it, same key. So F9 is
    // normalized to Escape's scancode right here, once, and everything
    // below proceeds exactly as if Escape had been pressed at whatever
    // step the wizard currently happens to be sitting on -- deliberately
    // preserving each step's own, already-nuanced Escape behavior (a full
    // cancel() from PickScope/PickDevice/Capture/Confirm, but only a one-
    // level "back" from ConfirmDelete/ConfirmRemap/ConfirmSecondStick)
    // rather than forcing a hard close from every step regardless -- the
    // least surprising choice, and zero risk of F9 ever discarding
    // progress Escape itself wouldn't have discarded from that same step.
    if (sc == SDL_SCANCODE_F9) sc = SDL_SCANCODE_ESCAPE;

    switch (step_) {
        case Step::PickScope:
            // See the top-of-file 2026-09-24 comment and beginWithScope()'s
            // own comment -- only reached when there's a real choice to
            // make (open() skips straight past this otherwise).
            if (sc == SDL_SCANCODE_UP || sc == SDL_SCANCODE_DOWN) {
                selectedScopeIndex_ = 1 - selectedScopeIndex_; // only two choices
            } else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
                beginWithScope(pendingLuaState_, selectedScopeIndex_ == 0 /* 0 = this game */);
            } else if (sc == SDL_SCANCODE_ESCAPE) {
                cancel();
            }
            return true;

        case Step::PickDevice:
            if (devices_.empty()) {
                if (sc == SDL_SCANCODE_ESCAPE) cancel();
                return true;
            }
            if (sc == SDL_SCANCODE_UP) {
                selectedDeviceIndex_ = (selectedDeviceIndex_ + static_cast<int>(devices_.size()) - 1)
                                        % static_cast<int>(devices_.size());
            } else if (sc == SDL_SCANCODE_DOWN) {
                selectedDeviceIndex_ = (selectedDeviceIndex_ + 1) % static_cast<int>(devices_.size());
            } else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
                startCapture(devices_[static_cast<size_t>(selectedDeviceIndex_)]);
            } else if (sc == SDL_SCANCODE_DELETE) {
                if (activeProfiles_->find(devices_[static_cast<size_t>(selectedDeviceIndex_)].guid) != nullptr) {
                    deleteCandidateIndex_ = selectedDeviceIndex_;
                    step_ = Step::ConfirmDelete;
                }
            } else if (sc == SDL_SCANCODE_ESCAPE) {
                cancel();
            }
            return true;

        case Step::ConfirmDelete:
            if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
                const DeviceInfo& d = devices_[static_cast<size_t>(deleteCandidateIndex_)];
                bool removed = activeProfiles_->remove(d.guid);
                if (removed) activeProfiles_->save(activePath_);
                std::fprintf(stderr, "[dirk] joystick wizard: profil %s pour \"%s\"\n",
                             removed ? "supprime" : "introuvable (deja absent)", d.name.c_str());
                deleteCandidateIndex_ = -1;
                step_ = Step::PickDevice;
            } else if (sc == SDL_SCANCODE_ESCAPE) {
                deleteCandidateIndex_ = -1;
                step_ = Step::PickDevice;
            }
            return true;

        case Step::ConfirmRemap:
            // Reached by pressing a button on a device already shown in
            // PickDevice (see handleJoyButton/handleGamepadButton) -- a
            // deliberate extra confirmation before committing to the full
            // whole capture flow, since (unlike Up/Down-then-Enter) a stray
            // button press could be accidental. Enter still needs a
            // keyboard, same as every other confirm in this wizard.
            if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
                startCapture(devices_[static_cast<size_t>(selectedDeviceIndex_)]);
            } else if (sc == SDL_SCANCODE_ESCAPE) {
                step_ = Step::PickDevice;
            }
            return true;

        case Step::ConfirmSecondStick:
            // 2026-09-24, Cyril: "il serait bien de proposer de configurer
            // un second stick apres le premier (certains joueurs aiment
            // jouer parfois sur le pad et parfois sur le stick gauche)".
            // Enter re-walks the 4 directional prompts a second time,
            // restricted to whichever input kind pass one DIDN'T use (see
            // hasButtonBinding/hasAxisOrHatBinding below); Escape skips
            // straight to prompt 4 (Start), same as declining anything
            // else in this wizard.
            if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
                bool anyButton = false, anyAxisOrHat = false;
                for (size_t i = 0; i < 4 && i < prompts().size(); ++i) {
                    int sw = prompts()[i].switchId;
                    if (hasButtonBinding(sw)) anyButton = true;
                    if (hasAxisOrHatBinding(sw)) anyAxisOrHat = true;
                }
                if (anyButton && !anyAxisOrHat) secondStickFilter_ = SecondStickFilter::AxisOnly;
                else if (anyAxisOrHat && !anyButton) secondStickFilter_ = SecondStickFilter::ButtonOnly;
                else secondStickFilter_ = SecondStickFilter::None; // mixed, or nothing bound at all -- accept whichever comes
                secondStickActive_ = true;
                secondStickAccepted_ = true;
                promptIndex_ = 0;
                step_ = Step::Capture;
                armDeadline();
                std::fprintf(stderr, "[dirk] joystick wizard: second stick demande (%s)\n",
                             secondStickFilter_ == SecondStickFilter::AxisOnly ? "stick attendu, D-pad ignore" :
                             secondStickFilter_ == SecondStickFilter::ButtonOnly ? "D-pad attendu, stick ignore" :
                             "l'un ou l'autre accepte");
            } else if (sc == SDL_SCANCODE_ESCAPE) {
                promptIndex_ = 4;
                step_ = Step::Capture;
                armDeadline();
            }
            return true;

        case Step::Capture:
            if (sc == SDL_SCANCODE_ESCAPE) {
                cancel();
            } else if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
                std::fprintf(stderr, "[dirk] joystick wizard: \"%s\" ignore manuellement (Entree)\n",
                             prompts()[promptIndex_].label);
                advancePrompt();
            } else if (sc == SDL_SCANCODE_BACKSPACE) {
                goBackPrompt();
            }
            return true;

        case Step::Confirm:
            if (sc == SDL_SCANCODE_RETURN || sc == SDL_SCANCODE_KP_ENTER) {
                finishSave();
            } else if (sc == SDL_SCANCODE_ESCAPE) {
                cancel();
            } else if (sc == SDL_SCANCODE_BACKSPACE) {
                if (!prompts().empty()) {
                    promptIndex_ = prompts().size() - 1;
                    clearPromptBinding(prompts()[promptIndex_].switchId);
                    step_ = Step::Capture;
                    armDeadline();
                }
            }
            return true;

        case Step::Inactive:
        default:
            return false;
    }
}

bool JoystickWizard::handleJoyButton(SDL_JoystickID which, int button, bool down) {
    // PickDevice: pressing a button on a device already shown in the list
    // jumps straight to it (see the top-of-file 2026-09-23 comment) --
    // kind-scoped to Raw devices only, since SDL also fires this exact
    // event for a Kind::Gamepad device's underlying joystick (verified
    // against a real SDL3 virtual gamepad, see that same comment); a
    // Kind::Gamepad device's button presses are picked up by
    // handleGamepadButton instead, never here.
    if (step_ == Step::PickDevice && down) {
        int idx = findDeviceIndex(which);
        if (idx >= 0 && devices_[static_cast<size_t>(idx)].kind == DeviceInfo::Kind::Raw) {
            selectedDeviceIndex_ = idx;
            step_ = Step::ConfirmRemap;
        }
        return true; // swallow either way -- never leaks to the paused game
    }
    if (step_ != Step::Capture || which != captureInstanceId_ || captureKind_ != DeviceInfo::Kind::Raw) {
        return true; // always swallow while the wizard is open
    }
    if (!down) return true; // only the press starts a capture, same edge-triggered convention as the rest of the engine
    if (button < 0 || button >= kRawJoyMaxButtons) return true;
    binding_.buttons[button] = prompts()[promptIndex_].switchId;
    std::fprintf(stderr, "[dirk] joystick wizard: \"%s\" -> bouton %d\n", prompts()[promptIndex_].label, button);
    advancePrompt();
    return true;
}

bool JoystickWizard::handleJoyHat(SDL_JoystickID which, int hat, Uint8 value) {
    if (step_ == Step::PickDevice) return true; // hat motion never selects a device -- see handleJoyButton
    if (step_ != Step::Capture || which != captureInstanceId_ || captureKind_ != DeviceInfo::Kind::Raw) return true;
    if (hat < 0 || hat >= kRawJoyMaxHats) return true;
    static constexpr Uint8 kHatBits[4] = {SDL_HAT_UP, SDL_HAT_RIGHT, SDL_HAT_DOWN, SDL_HAT_LEFT};
    for (Uint8 bit : kHatBits) {
        if ((value & bit) == 0) continue;
        auto& dir = binding_.hats[hat];
        int switchId = prompts()[promptIndex_].switchId;
        if (bit == SDL_HAT_UP) dir.up = switchId;
        else if (bit == SDL_HAT_RIGHT) dir.right = switchId;
        else if (bit == SDL_HAT_DOWN) dir.down = switchId;
        else dir.left = switchId;
        std::fprintf(stderr, "[dirk] joystick wizard: \"%s\" -> chapeau %d\n", prompts()[promptIndex_].label, hat);
        advancePrompt();
        return true; // one direction is enough even if this event is a diagonal (two bits set at once)
    }
    return true;
}

bool JoystickWizard::handleJoyAxis(SDL_JoystickID which, int axis, Sint16 value) {
    if (step_ == Step::PickDevice) return true; // axis motion never selects a device -- see handleJoyButton
    if (step_ != Step::Capture || which != captureInstanceId_ || captureKind_ != DeviceInfo::Kind::Raw) return true;
    if (axis < 0 || axis >= kRawJoyMaxAxes) return true;
    // Mirrors main.cpp's own kGamepadAxisDigitalThreshold -- same "stick
    // tilt far enough to count as a digital direction" reasoning, applied
    // here too so a resting/idle axis's small jitter never fires a
    // capture on its own.
    static constexpr Sint16 kThreshold = 16000;
    if (value <= -kThreshold) {
        binding_.axes[axis].neg = prompts()[promptIndex_].switchId;
        std::fprintf(stderr, "[dirk] joystick wizard: \"%s\" -> axe %d negatif\n", prompts()[promptIndex_].label, axis);
        advancePrompt();
    } else if (value >= kThreshold) {
        binding_.axes[axis].pos = prompts()[promptIndex_].switchId;
        std::fprintf(stderr, "[dirk] joystick wizard: \"%s\" -> axe %d positif\n", prompts()[promptIndex_].label, axis);
        advancePrompt();
    }
    return true;
}

bool JoystickWizard::handleGamepadButton(SDL_JoystickID which, int button, bool down) {
    // PickDevice: symmetric to handleJoyButton above, kind-scoped to
    // Gamepad devices only.
    if (step_ == Step::PickDevice && down) {
        int idx = findDeviceIndex(which);
        if (idx >= 0 && devices_[static_cast<size_t>(idx)].kind == DeviceInfo::Kind::Gamepad) {
            selectedDeviceIndex_ = idx;
            step_ = Step::ConfirmRemap;
        }
        return true;
    }
    if (step_ != Step::Capture || which != captureInstanceId_ || captureKind_ != DeviceInfo::Kind::Gamepad) {
        return true;
    }
    // Second-stick pass asking specifically for the stick (pass one
    // already used the D-pad) -- ignore button presses entirely so they
    // can't corrupt this pass; see Step::ConfirmSecondStick.
    if (secondStickActive_ && secondStickFilter_ == SecondStickFilter::AxisOnly) return true;
    if (!down) return true;
    // Same bound main.cpp's own runtime GAMEPAD_BUTTON_DOWN/UP handling
    // uses -- buttons past DPAD_RIGHT (MISC1, paddles, touchpad-click on
    // newer pads) have no SWITCH_* to map to either way.
    if (button < 0 || button > SDL_GAMEPAD_BUTTON_DPAD_RIGHT) return true;
    binding_.buttons[button] = prompts()[promptIndex_].switchId;
    std::fprintf(stderr, "[dirk] joystick wizard: \"%s\" -> bouton manette %d\n", prompts()[promptIndex_].label, button);
    advancePrompt();
    return true;
}

bool JoystickWizard::handleGamepadAxis(SDL_JoystickID which, int axis, Sint16 value) {
    if (step_ == Step::PickDevice) return true; // axis motion never selects a device -- see handleJoyButton
    if (step_ != Step::Capture || which != captureInstanceId_ || captureKind_ != DeviceInfo::Kind::Gamepad) {
        return true;
    }
    // Second-stick pass asking specifically for the D-pad (pass one
    // already used the stick) -- ignore axis motion entirely so it can't
    // corrupt this pass; see Step::ConfirmSecondStick.
    if (secondStickActive_ && secondStickFilter_ == SecondStickFilter::ButtonOnly) return true;
    // SDL_GamepadAxis only ever has 6 values (two sticks' X/Y + two
    // triggers) -- same bound main.cpp's own runtime GAMEPAD_AXIS_MOTION
    // handling documents.
    if (axis < 0 || axis >= 6) return true;
    // Same threshold as handleJoyAxis/main.cpp's kGamepadAxisDigitalThreshold.
    static constexpr Sint16 kThreshold = 16000;
    bool newNeg = value <= -kThreshold;
    bool newPos = value >= kThreshold;
    // Edge-triggered (see captureAxisNegPressed_/captureAxisPosPressed_'s
    // own comment in the header): only a false->true transition captures
    // anything -- a repeated report of the same already-crossed value
    // (which SDL's gamepad layer really does send, confirmed by testing)
    // must NOT capture again, or it would silently steal whatever prompt
    // advancePrompt() had already moved on to.
    if (newNeg && !captureAxisNegPressed_[axis]) {
        binding_.axes[axis].neg = prompts()[promptIndex_].switchId;
        std::fprintf(stderr, "[dirk] joystick wizard: \"%s\" -> axe manette %d negatif\n", prompts()[promptIndex_].label, axis);
        advancePrompt();
    }
    captureAxisNegPressed_[axis] = newNeg;
    if (newPos && !captureAxisPosPressed_[axis]) {
        binding_.axes[axis].pos = prompts()[promptIndex_].switchId;
        std::fprintf(stderr, "[dirk] joystick wizard: \"%s\" -> axe manette %d positif\n", prompts()[promptIndex_].label, axis);
        advancePrompt();
    }
    captureAxisPosPressed_[axis] = newPos;
    return true;
}

void JoystickWizard::tick(Uint32 nowTicks) {
    if (step_ == Step::PickDevice && devices_.empty()) {
        if (nowTicks >= deadlineTicks_) {
            std::fprintf(stderr, "[dirk] joystick wizard: rien a configurer, fermeture automatique\n");
            cancel();
        }
        return;
    }
    if (step_ != Step::Capture) return;
    if (nowTicks >= deadlineTicks_) {
        std::fprintf(stderr, "[dirk] joystick wizard: \"%s\" ignore (timeout -- absent sur cette borne ?)\n",
                     prompts()[promptIndex_].label);
        advancePrompt();
    }
}

void JoystickWizard::render(SDL_Renderer* renderer, int outW, int outH) {
    if (step_ == Step::Inactive) return;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 210);
    SDL_FRect full{0.0f, 0.0f, static_cast<float>(outW), static_cast<float>(outH)};
    SDL_RenderFillRect(renderer, &full);

    const SDL_Color white{255, 255, 255, 255};
    const SDL_Color dim{190, 190, 190, 255};
    const SDL_Color accent{255, 210, 90, 255};

    // Every string here is plain ASCII on purpose (no accents), even
    // though the TTF path below could render them fine -- SDL's built-in
    // SDL_RenderDebugText() fallback (used whenever font_ failed to load,
    // see the 2026-09-23 field report at the top of JoystickWizard.h)
    // only renders ASCII.
    //
    // Text is English. 2026-09-24: this was briefly translated to French
    // in response to Cyril's "UP/DOWN/LEFT/RIGHT plutot qu'en francais",
    // which turned out to be a misreading -- he clarified the same day
    // ("tout le texte de f9 doit etre en anglais") that he actually wants
    // ALL of this on-screen text in English; his original complaint was
    // about something else entirely (see prompts()'s own comment on his
    // labels, which stay exactly as he dictated them -- a deliberate
    // mix of French and English -- regardless of this). Reverted back to
    // English here. His alias-for-clarity request (see prompts()'s
    // comment on the QUIT/CONSOLE mismatch) was also clarified the same
    // day: he wants aliases used in the SAVED PROFILE FILE (see
    // JoystickProfile.cpp's use of switchIdToAliasName()), not shown in
    // this on-screen text -- so, unlike an earlier version of this
    // function, no alias is displayed below.
    std::vector<std::pair<std::string, SDL_Color>> content;

    switch (step_) {
        case Step::PickScope:
            content.push_back({"Joystick Setup (F9)", white});
            content.push_back({"", white});
            content.push_back({"Configure this joystick for:", white});
            content.push_back({"", white});
            content.push_back({std::string(selectedScopeIndex_ == 0 ? "> " : "  ") + "This game only",
                                selectedScopeIndex_ == 0 ? accent : white});
            content.push_back({std::string(selectedScopeIndex_ == 1 ? "> " : "  ") + "General (all games)",
                                selectedScopeIndex_ == 1 ? accent : white});
            content.push_back({"", white});
            content.push_back({"Up/Down: choose   Enter / Action 1: confirm   Escape / Action 2: cancel", dim});
            break;
        case Step::PickDevice:
            content.push_back({"Joystick Setup (F9)", white});
            content.push_back({std::string("Scope: ") + (scopeIsPerGame_ ? "this game" : "general, all games"), dim});
            content.push_back({"", white});
            if (devices_.empty()) {
                content.push_back({"No unrecognized joystick connected.", dim});
                content.push_back({"", white});
                content.push_back({"Closing automatically in a few seconds...", dim});
                content.push_back({"(or Escape / Action 2 to close now)", dim});
            } else {
                for (size_t i = 0; i < devices_.size(); ++i) {
                    bool sel = (static_cast<int>(i) == selectedDeviceIndex_);
                    bool has = activeProfiles_->find(devices_[i].guid) != nullptr;
                    const char* kindTag = devices_[i].kind == DeviceInfo::Kind::Gamepad ? "  [gamepad]" : "  [raw]";
                    std::string line = (sel ? "> " : "  ") + devices_[i].name + kindTag
                                        + (has ? "  (already configured)" : "");
                    content.push_back({line, sel ? accent : white});
                }
                content.push_back({"", white});
                content.push_back({"Up/Down: choose   Enter / Action 1: configure   Delete: erase profile   Escape / Action 2: close", dim});
                content.push_back({"(or just press a button on the device you want)", dim});
            }
            break;
        case Step::ConfirmDelete: {
            const std::string& name = devices_[static_cast<size_t>(deleteCandidateIndex_)].name;
            content.push_back({"Erase the profile for \"" + name + "\"?", white});
            content.push_back({"", white});
            content.push_back({"Enter / Action 1: confirm   Escape / Action 2: cancel", dim});
            break;
        }
        case Step::ConfirmRemap: {
            const std::string& name = devices_[static_cast<size_t>(selectedDeviceIndex_)].name;
            content.push_back({"Reconfigure \"" + name + "\"?", white});
            content.push_back({"", white});
            content.push_back({"Enter / Action 1: yes   Escape / Action 2: no, back to the list", dim});
            break;
        }
        case Step::ConfirmSecondStick:
            content.push_back({"Also configure the other input (D-pad / stick)", white});
            content.push_back({"for Up/Down/Left/Right?", white});
            content.push_back({"", white});
            content.push_back({"Enter / Action 1: yes   Escape / Action 2: no, skip", dim});
            break;
        case Step::Capture: {
            char stepLabel[64];
            std::snprintf(stepLabel, sizeof(stepLabel), "Step %d / %d", currentPromptNumber(), totalPromptCount());
            content.push_back({stepLabel, dim});
            // Second-stick pass (see Step::ConfirmSecondStick): reminds
            // the player which input this particular walk-through of
            // Up/Down/Left/Right wants, since it's easy to forget
            // partway through why they're being asked for "Up" again.
            if (secondStickActive_) {
                const char* which = secondStickFilter_ == SecondStickFilter::AxisOnly ? "stick"
                                     : secondStickFilter_ == SecondStickFilter::ButtonOnly ? "D-pad"
                                     : "second input";
                content.push_back({std::string("(second pass -- ") + which + ")", dim});
            }
            content.push_back({"", white});
            content.push_back({std::string("Press: ") + prompts()[promptIndex_].label, accent});
            content.push_back({"", white});
            Uint32 now = SDL_GetTicks();
            int remaining = deadlineTicks_ > now ? static_cast<int>((deadlineTicks_ - now + 999) / 1000) : 0;
            char timeLabel[32];
            std::snprintf(timeLabel, sizeof(timeLabel), "Time left: %ds", remaining);
            content.push_back({timeLabel, dim});
            content.push_back({"", white});
            content.push_back({"Enter: skip   Backspace: go back   Escape: cancel all   (no keyboard: wait to skip)", dim});
            break;
        }
        case Step::Confirm: {
            int bound = static_cast<int>(binding_.buttons.size());
            for (const auto& entry : binding_.axes) {
                if (entry.second.neg >= 0) ++bound;
                if (entry.second.pos >= 0) ++bound;
            }
            for (const auto& entry : binding_.hats) {
                if (entry.second.up >= 0) ++bound;
                if (entry.second.down >= 0) ++bound;
                if (entry.second.left >= 0) ++bound;
                if (entry.second.right >= 0) ++bound;
            }
            int total = totalPromptCount();
            char summary[96];
            std::snprintf(summary, sizeof(summary), "%d/%d assigned, %d skipped", bound, total, total - bound);
            content.push_back({"Setup complete for \"" + binding_.name + "\"", white});
            content.push_back({std::string("Scope: ") + (scopeIsPerGame_ ? "this game" : "general, all games"), dim});
            content.push_back({"", white});
            content.push_back({summary, dim});
            content.push_back({"", white});
            content.push_back({"Enter / Action 1: save   Backspace: redo the last one   Escape / Action 2: cancel (nothing saved)", dim});
            break;
        }
        case Step::Inactive:
            return;
    }

    if (font_) {
        const int lineH = TTF_GetFontHeight(font_) + 6;
        const int totalH = static_cast<int>(content.size()) * lineH;
        int y = (outH - totalH) / 2;
        for (const auto& [text, color] : content) {
            if (text.empty()) { y += lineH; continue; }
            SDL_Surface* surf = TTF_RenderText_Blended(font_, text.c_str(), 0, color);
            if (surf) {
                SDL_Texture* tex = SDL_CreateTextureFromSurface(renderer, surf);
                if (tex) {
                    // A line wider than the screen (the longer key hints on a
                    // 640x480 handheld) is shrunk to fit, same font.
                    const float maxW = static_cast<float>(outW) * 0.96f;
                    const float k = surf->w > maxW ? maxW / static_cast<float>(surf->w) : 1.0f;
                    const float w = surf->w * k, h = surf->h * k;
                    SDL_FRect dst{(static_cast<float>(outW) - w) / 2.0f, static_cast<float>(y), w, h};
                    SDL_RenderTexture(renderer, tex, nullptr, &dst);
                    SDL_DestroyTexture(tex);
                }
                SDL_DestroySurface(surf);
            }
            y += lineH;
        }
    } else {
        // No TTF font available (see the 2026-09-23 field report at the
        // top of JoystickWizard.h) -- SDL_RenderDebugText() needs no
        // external file at all, so this guarantees the player always sees
        // SOMETHING readable here, never a silent dark screen. Scaled up
        // (SDL_SetRenderScale) since its native 8px glyphs are too small
        // to read from a real cabinet's typical viewing distance; text
        // coordinates below are in the SCALED coordinate space, matching
        // how SDL_RenderDebugText's own x/y are documented to work under
        // an active render scale.
        constexpr float kScale = 3.0f;
        constexpr int kCharW = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE;
        const int lineH = kCharW + 4;
        const int totalH = static_cast<int>(content.size()) * lineH;
        SDL_SetRenderScale(renderer, kScale, kScale);
        int y = static_cast<int>((outH / kScale) - totalH) / 2;
        for (const auto& [text, color] : content) {
            if (!text.empty()) {
                int x = static_cast<int>((outW / kScale - static_cast<float>(text.size() * kCharW)) / 2.0f);
                SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
                SDL_RenderDebugText(renderer, static_cast<float>(x), static_cast<float>(y), text.c_str());
            }
            y += lineH;
        }
        SDL_SetRenderScale(renderer, 1.0f, 1.0f); // never leak the scale into the rest of the frame
    }
}

} // namespace dirk
