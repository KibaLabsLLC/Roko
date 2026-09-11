/* KibaOS OOBE — GTK4 + libadwaita, white-card design language.
 * Backend: /usr/local/bin/kibaos-oobe-backend (C, libkibadisk). */

using Gtk;
using Adw;
using Gee;

public struct OobeSummaryItem {
    public string key;
    public string val;
}

public class KibaOOBE : Adw.Application {
    private Adw.ApplicationWindow window;
    private Adw.NavigationView    nav_view;
    private string selected_disk   = "";
    private string install_mode    = "erase"; // "erase" or "alongside" (dual boot)
    private string selected_locale = "en_US.UTF-8";
    private string selected_keymap = "us";
    private string hostname_value  = "kibaos";
    private string username_value  = "";
    private string password_value  = "";
    private bool   is_oem_mode     = false;
    private const string OEM_MARKER = "/etc/kibaos/oem-pending";

    // ── Language ──────────────────────────────────────────────────────
    // Only affects the OOBE's own UI text. The locale picked on the
    // Language & Keyboard page is a separate thing (that's what the
    // installed system will use).
    private string ui_lang = "en";

    // tiny inline translator — t("English", "Türkçe") right at each call
    // site instead of a separate lookup table, so a string and its
    // translation stay glued together in the source instead of drifting
    // apart in two different files.
    private string t (string en, string tr, string pl) {
        return ui_lang == "tr" ? tr : ui_lang == "pl" ? pl : en;
    }

    // ── Dark mode ─────────────────────────────────────────────────────
    private bool is_dark = false;

    private void apply_dark_mode () {
        if (is_dark) window.add_css_class ("dark");
        else window.remove_css_class ("dark");
    }

    // ── VM detection ──────────────────────────────────────────────────
    // No longer used to block installation (VDI/VMDK/qcow2 handling has
    // been solid enough in practice that the original blanket refusal
    // wasn't buying anything except friction for people testing/running
    // KibaOS in a VM) -- still used below for OEM-mode detection, since
    // a VM's virtual disk can spuriously look like "already on the
    // computer" the same way a real OEM-preloaded disk does, and we
    // don't want that misfiring in a test VM.
    private bool is_running_in_vm () {
        string out_str = "", err_str = "";
        int status = 0;
        try {
            GLib.Process.spawn_command_line_sync (
                "systemd-detect-virt -q", out out_str, out err_str, out status);
        } catch (GLib.SpawnError e) { return false; }
        // systemd-detect-virt exits 0 when it detects a VM/container,
        // 1 when running on bare metal.
        return GLib.Process.if_exited (status) && GLib.Process.exit_status (status) == 0;
    }

    // ── Helpers ────────────────────────────────────────────────────────
    private string strip_partition_suffix (string n) {
        string r = n;
        try {
            r = /[0-9]+$/.replace (r, -1, 0, "");
            if (r.has_prefix ("nvme")) r = /p$/.replace (r, -1, 0, "");
        } catch (GLib.RegexError e) {}
        return r;
    }

    private bool detect_already_on_computer () {
        if (GLib.FileUtils.test (OEM_MARKER, GLib.FileTest.EXISTS)) return true;
        string src = "";
        try { GLib.Process.spawn_command_line_sync (
                "findmnt -n -o SOURCE /run/archiso/bootmnt", out src); }
        catch (GLib.SpawnError e) { return false; }
        src = src.strip ();
        if (src == "") return false;
        string removable_path = "/sys/block/%s/removable".printf (
            strip_partition_suffix (GLib.Path.get_basename (src)));
        string removable_content = "";
        try { GLib.FileUtils.get_contents (removable_path, out removable_content); }
        catch (GLib.FileError e) { return false; }
        return removable_content.strip () == "0";
    }

    public KibaOOBE () {
        Object (application_id: "io.kibaos.oobe", flags: ApplicationFlags.FLAGS_NONE);
    }

    protected override void activate () {
        bool in_vm = is_running_in_vm ();
        is_oem_mode = !in_vm && detect_already_on_computer ();
        window = new Adw.ApplicationWindow (this) {
            default_width  = 1280,
            default_height = 800,
            fullscreened   = true,
            decorated      = false,
            resizable      = false,
            title          = in_vm ? "KibaOS Setup"
                             : is_oem_mode ? "Finish Setting Up KibaOS" : "KibaOS Setup"
        };
        window.add_css_class ("kibaos-oobe-window");
        // No decorations means no close button already, but Alt+F4/compositor
        // shortcuts can still fire a close request -- OOBE isn't something
        // you dismiss out from under yourself mid-install, same as Windows'
        // setup flow never gives you a way out either. Block it outright.
        window.close_request.connect (() => { return true; });
        // Belt-and-suspenders: if a compositor keybinding (or anything else)
        // ever drops fullscreen out from under us, snap straight back into
        // it instead of leaving OOBE sitting in a windowed state.
        window.notify["fullscreened"].connect (() => {
            if (!window.fullscreened) window.fullscreen ();
        });
        // Default to light mode; the toggle on every page still lets the
        // user switch to dark from there.
        Adw.StyleManager.get_default ().color_scheme = Adw.ColorScheme.FORCE_LIGHT;
        is_dark = false;
        apply_dark_mode ();
        nav_view = new Adw.NavigationView ();
        window.set_content (nav_view);
        load_css ();
        nav_view.push (is_oem_mode ? build_locale_page () : build_welcome_page ());
        window.present ();
    }

    private void load_css () {
        var p = new Gtk.CssProvider ();
        p.load_from_path ("/usr/share/kibaos-oobe/oobe.css");
        Gtk.StyleContext.add_provider_for_display (
            Gdk.Display.get_default (), p, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    private delegate void NextAction ();

    // ── Page chrome ────────────────────────────────────────────────────
    // step_index / step_total drive the dot-indicator at the top of each card.
    private Adw.NavigationPage make_page (
            string title, Gtk.Widget content,
            string? next_label, NextAction? on_next,
            bool hide_back   = false,
            int  step_index  = 0,
            int  step_total  = 0) {

        // Outer overlay = full-screen canvas
        var root = new Gtk.Overlay ();
        root.add_css_class ("oobe-background");

        // Frosted card
        var card = new Gtk.Box (Gtk.Orientation.VERTICAL, 0) {
            halign = Gtk.Align.CENTER,
            valign = Gtk.Align.CENTER,
            width_request = 640
        };
        card.add_css_class ("oobe-card");

        // ── Step-dots (shown when step_total > 1) ──────────────────────
        if (step_total > 1) {
            var dots_row = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 6) {
                halign = Gtk.Align.CENTER,
                margin_bottom = 10
            };
            for (int i = 0; i < step_total; i++) {
                var dot = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 0) {};
                dot.add_css_class ("oobe-step-dot");
                if (i == step_index) dot.add_css_class ("oobe-step-dot-active");
                dots_row.append (dot);
            }
            card.append (dots_row);

            var step_label = new Gtk.Label (
                t ("Step %d of %d", "Adım %d / %d", "Krok %d z %d")
                    .printf (step_index + 1, step_total)) {
                halign = Gtk.Align.CENTER,
                margin_bottom = 16
            };
            step_label.add_css_class ("oobe-step-label");
            card.append (step_label);
        }

        // Content + nav row wrapped in a box with padding
        var inner = new Gtk.Box (Gtk.Orientation.VERTICAL, 24);
        inner.add_css_class ("oobe-inner");
        inner.append (content);

        // Nav row
        var nav_row = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 10) {
            halign = Gtk.Align.FILL,
            margin_top = 8
        };
        nav_row.add_css_class ("oobe-nav-row");

        // Spacer
        var spacer = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 0) { hexpand = true };
        nav_row.append (spacer);

        if (!hide_back && nav_view.get_navigation_stack ().get_n_items () > 1) {
            var back_btn = new Gtk.Button.with_label (t ("Back", "Geri", "Wstecz"));
            back_btn.add_css_class ("oobe-secondary-button");
            back_btn.clicked.connect (() => nav_view.pop ());
            nav_row.append (back_btn);
        }
        if (next_label != null) {
            var next_btn = new Gtk.Button.with_label (next_label);
            next_btn.add_css_class ("oobe-primary-button");
            if (on_next != null) {
                NextAction action = on_next;
                next_btn.clicked.connect (() => { action (); });
            }
            nav_row.append (next_btn);
        }
        inner.append (nav_row);
        card.append (inner);
        root.add_overlay (card);

        // KibaOS wordmark top-left
        var brand = new Gtk.Label ("KibaOS") {
            halign = Gtk.Align.START,
            valign = Gtk.Align.START,
            margin_start = 36,
            margin_top   = 32
        };
        brand.add_css_class ("oobe-brand");
        root.add_overlay (brand);

        // Corner controls top-right: language toggle + dark-mode toggle.
        // Live on every screen (not just Welcome) so switching either one
        // doesn't force a trip back to page 1.
        var corner = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 8) {
            halign       = Gtk.Align.END,
            valign       = Gtk.Align.START,
            margin_end   = 28,
            margin_top   = 24
        };

        var lang_btn = new Gtk.Button.with_label (
            ui_lang == "en" ? "TR" : ui_lang == "tr" ? "PL" : "EN");
        lang_btn.add_css_class ("oobe-corner-button");
        // Tooltip is always phrased in the CURRENT language, naming the NEXT
        // one in the cycle (en -> tr -> pl -> en) -- matches the original
        // en/tr behaviour, just extended to a third stop.
        lang_btn.tooltip_text = ui_lang == "en" ? "Switch to Turkish"
                               : ui_lang == "tr" ? "Lehçeye geç"
                               : "Przełącz na angielski";
        lang_btn.clicked.connect (() => {
            ui_lang = ui_lang == "en" ? "tr" : ui_lang == "tr" ? "pl" : "en";
            refresh_current_page ();
        });
        corner.append (lang_btn);

        var dark_btn = new Gtk.Button.with_label (
            is_dark ? t ("Light", "Açık", "Jasny") : t ("Dark", "Koyu", "Ciemny"));
        dark_btn.add_css_class ("oobe-corner-button");
        dark_btn.tooltip_text = is_dark
            ? t ("Switch to light mode", "Açık moda geç", "Przełącz na jasny motyw")
            : t ("Switch to dark mode", "Koyu moda geç", "Przełącz na ciemny motyw");
        dark_btn.clicked.connect (() => {
            is_dark = !is_dark;
            apply_dark_mode ();
            refresh_current_page ();
        });
        corner.append (dark_btn);

        root.add_overlay (corner);

        return new Adw.NavigationPage (root, title);
    }

    // Rebuilds whatever page is currently on top of the nav stack, so a
    // language/theme toggle is reflected immediately instead of only on
    // the next forward/back navigation.
    private delegate Adw.NavigationPage PageBuilder ();
    private void refresh_current_page () {
        var stack = nav_view.get_navigation_stack ();
        uint n = stack.get_n_items ();
        if (n == 0) return;
        var current = stack.get_item (n - 1) as Adw.NavigationPage;
        string tag = current != null ? current.title : "";
        PageBuilder rebuild;
        switch (tag) {
            case "Welcome":          rebuild = build_welcome_page; break;
            case "Wi-Fi":            rebuild = build_wifi_page; break;
            case "Language":         rebuild = build_locale_page; break;
            case "Account":          rebuild = build_account_page; break;
            case "Confirm":          rebuild = build_confirm_page; break;
            case "Done":             rebuild = build_done_page; break;
            default: return; // Storage / Install Mode / Installing carry
                              // per-instance state that isn't worth
                              // reconstructing from scratch mid-flow.
        }
        nav_view.pop ();
        nav_view.push (rebuild ());
    }

    // ── Heading ─────────────────────────────────────────────────────────
    private Gtk.Widget oobe_heading (string text, string? subtitle = null) {
        var box = new Gtk.Box (Gtk.Orientation.VERTICAL, 6);
        var label = new Gtk.Label (text);
        label.add_css_class ("oobe-title");
        label.halign = Gtk.Align.START;
        label.wrap   = true;
        box.append (label);
        if (subtitle != null) {
            var sub = new Gtk.Label (subtitle);
            sub.add_css_class ("oobe-subtitle");
            sub.halign = Gtk.Align.START;
            sub.wrap   = true;
            box.append (sub);
        }
        return box;
    }

    // ══════════════════════════════════════════════════════════════════
    // Page 1: Welcome
    // ══════════════════════════════════════════════════════════════════
    private Adw.NavigationPage build_welcome_page () {
        var content = new Gtk.Box (Gtk.Orientation.VERTICAL, 20) {
            halign = Gtk.Align.CENTER
        };

        // Logo — centered above the greeting, Apple's own "Hello" screen
        // skips a brand mark entirely and lets the greeting carry the
        // whole moment, but KibaOS keeps a small centered one here since
        // the persistent corner wordmark is easy to miss on a first boot.
        var logo = new Gtk.Image.from_file ("/usr/share/kibaos/installer-logo.png") {
            pixel_size = 64,
            halign     = Gtk.Align.CENTER
        };
        content.append (logo);

        // The greeting itself -- plain system font, same treatment as
        // every other page title. HIG is explicit that custom/script
        // faces are for branding moments or "an immersive gaming
        // experience," not a plain OS installer -- one typeface used
        // consistently reads calmer and more native than a flourish here.
        var greeting = new Gtk.Label (
            t ("Welcome", "Hoş geldiniz", "Witamy")) {
            halign = Gtk.Align.CENTER,
            justify = Gtk.Justification.CENTER
        };
        greeting.add_css_class ("oobe-welcome-greeting");
        content.append (greeting);

        var subtitle = new Gtk.Label (
            t ("Let's get your system set up. This should only take a few minutes.",
               "Sisteminizi kuralım. Bu işlem yalnızca birkaç dakika sürecek.",
               "Skonfigurujmy Twój system. To zajmie tylko kilka minut.")) {
            halign = Gtk.Align.CENTER,
            justify = Gtk.Justification.CENTER,
            margin_top = 4
        };
        subtitle.add_css_class ("oobe-subtitle");
        content.append (subtitle);

        var nav_row = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 10) {
            halign = Gtk.Align.CENTER,
            margin_top = 24
        };
        var try_btn = new Gtk.Button.with_label (t ("Try KibaOS", "KibaOS'u Dene", "Wypróbuj KibaOS"));
        try_btn.add_css_class ("oobe-secondary-button");
        try_btn.tooltip_text = t ("Explore the live desktop without installing anything yet.",
            "Henüz hiçbir şey kurmadan canlı masaüstünü keşfedin.",
            "Poznaj system na żywo, nie instalując jeszcze niczego.");
        try_btn.clicked.connect (() => { this.quit (); });
        nav_row.append (try_btn);
        content.append (nav_row);

        return make_page ("Welcome", content, t ("Get Started", "Başla", "Rozpocznij"), () => {
            nav_view.push (build_wifi_page ());
        }, true, 0, 0);
    }

    // ══════════════════════════════════════════════════════════════════
    // Page 2: Wi-Fi  (animated icon, network list, actual connection)
    // ══════════════════════════════════════════════════════════════════
    private Adw.NavigationPage build_wifi_page () {
        var content = new Gtk.Box (Gtk.Orientation.VERTICAL, 24);

        // Animated Wi-Fi icon drawn on a DrawingArea
        var canvas = new Gtk.DrawingArea () {
            width_request  = 120,
            height_request = 120,
            halign         = Gtk.Align.CENTER
        };
        canvas.add_css_class ("oobe-wifi-canvas");

        double[] tick   = { 0.0 };
        double[] wiggle = { 0.0 };
        uint[]   src_id = { 0 };

        canvas.set_draw_func ((da, cr, w, h) => {
            double t      = tick[0];
            double cx     = w / 2.0;
            double cy     = h / 2.0 + 8;
            double r1     = 14.0, r2 = 26.0, r3 = 38.0;
            double sw     = 4.5;

            double a3 = double.max (0, double.min (1, t * 3));
            double a2 = double.max (0, double.min (1, t * 3 - 0.6));
            double a1 = double.max (0, double.min (1, t * 3 - 1.2));

            cr.set_line_width (sw);
            cr.set_line_cap (Cairo.LineCap.ROUND);
            double start_angle = Math.PI * (1.0 + 0.18);
            double end_angle   = Math.PI * (2.0 - 0.18);

            cr.set_source_rgba (0.0, 0.60, 0.80, a3 * 0.85);
            cr.arc (cx, cy, r3, start_angle, end_angle);
            cr.stroke ();

            cr.set_source_rgba (0.0, 0.60, 0.80, a2 * 0.90);
            cr.arc (cx, cy, r2, start_angle, end_angle);
            cr.stroke ();

            cr.set_source_rgba (0.0, 0.60, 0.80, a1 * 0.95);
            cr.arc (cx, cy, r1, start_angle, end_angle);
            cr.stroke ();

            double dp    = (t * 1.4) % 1.0;
            double rip_r = 6.0 + dp * 18.0;
            double rip_a = (1.0 - dp) * 0.55;

            cr.set_source_rgba (0.0, 0.60, 0.80, rip_a);
            cr.set_line_width (2.0);
            cr.arc (cx, cy + r3 - 2.0 + wiggle[0] * 3.0, rip_r, 0, 2 * Math.PI);
            cr.stroke ();

            cr.set_source_rgba (0.0, 0.60, 0.80, 1.0);
            cr.arc (cx, cy + r3 - 2.0 + wiggle[0] * 3.0, 5.5, 0, 2 * Math.PI);
            cr.fill ();
        });

        src_id[0] = GLib.Timeout.add (16, () => {
            tick[0]   = (tick[0] + 0.012) % 1.0;
            wiggle[0] = Math.sin (tick[0] * Math.PI * 6.0) * 0.4;
            canvas.queue_draw ();
            return GLib.Source.CONTINUE;
        });

        canvas.destroy.connect (() => {
            if (src_id[0] != 0) { GLib.Source.remove (src_id[0]); src_id[0] = 0; }
        });

        content.append (canvas);
        content.append (oobe_heading (
            t ("Connect to Wi-Fi", "Wi-Fi'ye Bağlan", "Połącz z Wi-Fi"),
            t ("Choose a network to continue. You can also skip this step.",
               "Devam etmek için bir ağ seçin. Bu adımı atlayabilirsiniz de.",
               "Wybierz sieć, aby kontynuować. Możesz też pominąć ten krok.")));

        // Status label shown below the list ("Connecting…", "Connected ✓", errors)
        var status_label = new Gtk.Label ("") {
            halign = Gtk.Align.CENTER,
            wrap   = true
        };
        status_label.add_css_class ("oobe-subtitle");

        // Network is now required (codec install during finalize needs it,
        // and skipping used to leave people on a system with no way to
        // reach the mirrors afterward either). Next just refuses to
        // navigate until this is true -- either flipped by
        // do_connect_async below, or here already if wired/some other
        // connection is already up (don't force a Wi-Fi-specific flow on
        // someone plugged into Ethernet).
        bool[] connected_box = { false };
        try {
            string state_out = "";
            GLib.Process.spawn_command_line_sync (
                "nmcli -t -f STATE general status", out state_out);
            if (state_out.strip () == "connected") {
                connected_box[0] = true;
                status_label.label = t ("Already connected ✓", "Zaten bağlı ✓", "Już połączono ✓");
            }
        } catch (GLib.SpawnError e) {}

        // ── Discover the first NetworkManager-managed wireless device ─────
        string wifi_dev = "";
        try {
            string dev_out = "";
            GLib.Process.spawn_command_line_sync (
                "nmcli -t -f DEVICE,TYPE device status", out dev_out);
            foreach (var line in dev_out.split ("\n")) {
                var trimmed = line.strip ();
                if (trimmed == "") continue;
                var cols = trimmed.split (":");
                if (cols.length >= 2 && cols[1] == "wifi") { wifi_dev = cols[0]; break; }
            }
        } catch (GLib.SpawnError e) {}

        // Capture for closures
        string[] dev_box = { wifi_dev };

        // ── Build network list ────────────────────────────────────────────
        var list_box = new Gtk.ListBox ();
        list_box.add_css_class ("oobe-list");
        list_box.selection_mode = Gtk.SelectionMode.SINGLE;

        // Each row stores its SSID and whether it needs a password in widget data.
        // We use a simple parallel arrays approach since Vala/GTK4 has no
        // set_data on widgets without GObject subclassing tricks. Boxed
        // (double-array) so refresh_networks() below can reassign the
        // contents and have row_activated's closure (created once, further
        // down) see the updated values on every rescan -- reassigning a
        // plain unboxed local wouldn't be visible from a closure created
        // before the reassignment.
        // NOTE: `string[][] x = { {} };` is NOT valid Vala -- "stacked
        // array" literals like that aren't supported by the compiler
        // (confirmed: valac 0.56.19 rejects it outright). Gee.ArrayList
        // gives the same "closures see later mutations" property since
        // it's a reference type -- refresh_networks() below just
        // clear()/add()s into the same list object instead of reassigning
        // a boxed array slot.
        var ssid_box    = new Gee.ArrayList<string> ();
        var secured_box = new Gee.ArrayList<bool> ();

        // Set while a password dialog is open or a connection attempt is
        // in flight, so the periodic rescan below doesn't yank the row
        // list out from under someone mid-pick or mid-typing.
        bool[] scan_paused_box = { false };

        // Pulled out into its own function so the periodic timer further
        // down can just call this again instead of duplicating the whole
        // scan-and-render pass. Safe to call repeatedly: it fully rebuilds
        // list_box's rows and ssid_box/secured_box each time rather than
        // diffing, which is fine at Wi-Fi-scan-list sizes.
        void refresh_networks () {
            if (scan_paused_box[0]) return;

            string raw_nets = "";
            if (dev_box[0] != "") {
                try {
                    string scan_out = "";
                    GLib.Process.spawn_command_line_sync (
                        "nmcli device wifi rescan ifname %s".printf (dev_box[0]), out scan_out);
                    // Colon-separated, one AP per line: SSID:SECURITY:SIGNAL
                    // (nmcli backslash-escapes literal colons inside the SSID field).
                    GLib.Process.spawn_command_line_sync (
                        "nmcli -t -f SSID,SECURITY,SIGNAL device wifi list ifname %s".printf (dev_box[0]),
                        out raw_nets);
                } catch (GLib.SpawnError e) {}
            }

            // Clear whatever's there from the previous pass before
            // repopulating -- including the "No networks found" placeholder
            // row, if that's what's currently shown.
            Gtk.Widget? child = list_box.get_first_child ();
            while (child != null) {
                var next = child.get_next_sibling ();
                list_box.remove (child);
                child = next;
            }
            ssid_box.clear ();
            secured_box.clear ();

            var seen = new Gee.HashSet<string> ();
            bool any = false;
            foreach (var line in raw_nets.split ("\n")) {
                var trimmed = line.strip ();
                if (trimmed == "") continue;
                // Split on unescaped colons only (nmcli escapes literal ':' in
                // field values as '\:').
                var cols = GLib.Regex.split_simple ("(?<!\\\\):", trimmed);
                if (cols.length < 3) continue;
                string ssid = cols[0].replace ("\\:", ":").strip ();
                if (ssid == "" || seen.contains (ssid)) continue;
                seen.add (ssid);
                string security    = cols[1].strip ().down ();
                string signal_str  = cols[2].strip ();
                bool   secured     = security != "" && security != "--";
                int    pct         = int.parse (signal_str);
                string signal_pct  = "%d%%".printf (int.max (0, int.min (100, pct)));

                // Choose a text signal-bar glyph based on nmcli's 0-100 signal
                // percentage, instead of an icon-theme lookup.
                string signal_bars;
                if      (pct >= 80) signal_bars = "▂▄▆█";
                else if (pct >= 55) signal_bars = "▂▄▆";
                else if (pct >= 30) signal_bars = "▂▄";
                else                signal_bars = "▂";

                var row = new Adw.ActionRow () {
                    title         = ssid,
                    subtitle      = "%s\n%s".printf (signal_pct,
                                        secured
                                            ? t ("Secured", "Güvenli", "Zabezpieczona")
                                            : t ("Open", "Açık", "Otwarta")),
                    subtitle_lines = 2,
                    activatable   = true
                };
                row.add_prefix (new Gtk.Label (signal_bars) { css_classes = { "oobe-signal-glyph" } });
                list_box.append (row);

                ssid_box.add (ssid);
                secured_box.add (secured);
                any = true;
            }
            if (!any) {
                var row = new Adw.ActionRow () {
                    title = t ("No networks found nearby", "Yakında ağ bulunamadı", "Nie znaleziono pobliskich sieci")
                };
                list_box.append (row);
            }
        }

        refresh_networks ();

        content.append (list_box);
        content.append (status_label);

        // Periodic rescan so networks that come into/out of range while
        // this page is up actually show up, instead of only ever
        // reflecting a single scan taken the instant the page was built.
        // Torn down via list_box.destroy so it stops firing (and touching
        // a dead widget) once the user navigates past this page.
        uint[] refresh_timer_box = { 0 };
        refresh_timer_box[0] = GLib.Timeout.add_seconds (5, () => {
            refresh_networks ();
            return GLib.Source.CONTINUE;
        });
        list_box.destroy.connect (() => {
            if (refresh_timer_box[0] != 0) {
                GLib.Source.remove (refresh_timer_box[0]);
                refresh_timer_box[0] = 0;
            }
        });

        // ── Row activation: password dialog → nmcli connect ──────────────
        // Captures: dev_box, ssid_box, secured_box, status_label, window,
        // scan_paused_box
        list_box.row_activated.connect ((row) => {
            int idx = row.get_index ();
            if (idx < 0 || idx >= ssid_box.size) return;

            string ssid    = ssid_box[idx];
            bool   secured = secured_box[idx];

            if (secured) {
                // ── Password dialog ───────────────────────────────────────
                // Pause the 5s rescan for as long as this dialog (or the
                // resulting connect attempt) is live -- a scan mid-typing
                // would rebuild list_box's rows out from under the user.
                scan_paused_box[0] = true;

                var dialog = new Adw.MessageDialog (window,
                    t ("Enter Wi-Fi Password", "Wi-Fi Şifresini Girin", "Wprowadź hasło Wi-Fi"),
                    t ("""Enter the password for "%s".""",
                       """"%s" ağının şifresini girin.""",
                       "Wprowadź hasło dla „%s”.").printf (ssid));

                var pw_entry = new Gtk.PasswordEntry () {
                    show_peek_icon = true,
                    placeholder_text = t ("Password", "Şifre", "Hasło")
                };
                pw_entry.add_css_class ("oobe-entry");
                dialog.set_extra_child (pw_entry);

                dialog.add_response ("cancel", t ("Cancel", "İptal", "Anuluj"));
                dialog.add_response ("connect", t ("Connect", "Bağlan", "Połącz"));
                dialog.set_response_appearance ("connect", Adw.ResponseAppearance.SUGGESTED);
                dialog.set_default_response ("connect");
                dialog.set_close_response ("cancel");

                // Allow pressing Enter in the password field to confirm
                pw_entry.activate.connect (() => {
                    dialog.response ("connect");
                });

                dialog.response.connect ((resp) => {
                    if (resp != "connect") { dialog.destroy (); scan_paused_box[0] = false; return; }
                    string password = pw_entry.get_text ();
                    dialog.destroy ();

                    if (password == "") {
                        status_label.remove_css_class ("oobe-subtitle");
                        status_label.add_css_class ("oobe-error");
                        status_label.label = t ("Password cannot be empty.",
                                                 "Şifre boş olamaz.",
                                                 "Hasło nie może być puste.");
                        scan_paused_box[0] = false;
                        return;
                    }

                    status_label.remove_css_class ("oobe-error");
                    status_label.add_css_class ("oobe-subtitle");
                    status_label.label = t ("Connecting to %s…", "%s ağına bağlanıyor…", "Łączenie z %s…").printf (ssid);
                    do_connect_async (dev_box[0], ssid, password, status_label, connected_box);
                });

                dialog.present ();

            } else {
                // ── Open network — connect directly ───────────────────────
                scan_paused_box[0] = true;
                status_label.label = t ("Connecting to %s…", "%s ağına bağlanıyor…", "Łączenie z %s…").printf (ssid);
                do_connect_async (dev_box[0], ssid, null, status_label, connected_box);
            }
        });

        return make_page ("Wi-Fi", content, t ("Next", "İleri", "Dalej"), () => {
            if (!connected_box[0]) {
                status_label.remove_css_class ("oobe-subtitle");
                status_label.add_css_class ("oobe-error");
                status_label.label = t ("Connect to a network to continue -- KibaOS needs it to fetch media codecs during install.",
                                         "Devam etmek için bir ağa bağlanın -- KibaOS, kurulum sırasında medya kodeklerini almak için buna ihtiyaç duyar.",
                                         "Połącz się z siecią, aby kontynuować -- KibaOS potrzebuje jej do pobrania kodeków multimedialnych podczas instalacji.");
                return;
            }
            nav_view.push (build_locale_page ());
        }, false, 1, 6);
    }

    // ── Async connect helper ─────────────────────────────────────────────────
    // Runs `nmcli device wifi connect <ssid> [password <pw>] ifname <dev>` in
    // a background GLib.Thread so the GTK main loop stays responsive, then
    // polls `nmcli device show <dev>` on the main thread for up to 15 s to
    // confirm association. Updates status_label on each step.
    private void do_connect_async (string dev, string ssid,
                                    string? password,
                                    Gtk.Label status_label,
                                    bool[] connected_box) {
        // Build argv — no shell, no quoting/injection surface
        string[] argv_arr;
        if (password != null) {
            argv_arr = { "nmcli", "device", "wifi", "connect", ssid,
                         "password", password, "ifname", dev };
        } else {
            argv_arr = { "nmcli", "device", "wifi", "connect", ssid,
                         "ifname", dev };
        }

        // Kick the blocking nmcli call off the main thread.
        // When it finishes, schedule the polling phase back on the main loop.
        string[]  argv_copy   = argv_arr;
        string    dev_copy    = dev;
        string    ssid_copy   = ssid;
        unowned Gtk.Label lbl = status_label;

        new GLib.Thread<void> ("kibaos-wifi-connect", () => {
            bool   ok  = false;
            string err = "";
            try {
                int exit_status = 0;
                GLib.Process.spawn_sync (
                    null, argv_copy, null,
                    GLib.SpawnFlags.SEARCH_PATH         |
                    GLib.SpawnFlags.STDOUT_TO_DEV_NULL  |
                    GLib.SpawnFlags.STDERR_TO_DEV_NULL,
                    null, null, null, out exit_status);
                ok  = (exit_status == 0);
                err = ok ? "" : "nmcli exit %d".printf (exit_status);
            } catch (GLib.SpawnError e) {
                err = e.message;
            }

            // Marshal result back to the GTK main thread
            bool   ok_f  = ok;
            string err_f = err;
            GLib.Idle.add (() => {
                if (!ok_f) {
                    lbl.remove_css_class ("oobe-subtitle");
                    lbl.add_css_class ("oobe-error");
                    lbl.label = t ("Could not connect: %s", "Bağlanılamadı: %s", "Nie udało się połączyć: %s").printf (err_f);
                    return GLib.Source.REMOVE;
                }
                // Start polling for association on the main thread
                lbl.remove_css_class ("oobe-error");
                lbl.add_css_class ("oobe-subtitle");
                lbl.label = t ("Verifying connection…", "Bağlantı doğrulanıyor…", "Weryfikowanie połączenia…");
                int[] attempts = { 0 };
                GLib.Timeout.add (500, () => {
                    attempts[0]++;
                    bool associated = false;
                    try {
                        string show_out = "";
                        GLib.Process.spawn_command_line_sync (
                            "nmcli -t -f GENERAL.STATE device show %s".printf (dev_copy),
                            out show_out);
                        associated = show_out.down ().contains ("connected");
                    } catch (GLib.SpawnError e) {}

                    if (associated) {
                        lbl.label = t ("Connected to %s ✓", "%s ağına bağlanıldı ✓", "Połączono z %s ✓").printf (ssid_copy);
                        connected_box[0] = true;
                        return GLib.Source.REMOVE;
                    }
                    if (attempts[0] >= 30) {   // 30 × 500 ms = 15 s
                        lbl.remove_css_class ("oobe-subtitle");
                        lbl.add_css_class ("oobe-error");
                        lbl.label = t ("Timed out — check the password and try again.",
                                        "Zaman aşımı — şifreyi kontrol edip tekrar deneyin.",
                                        "Przekroczono czas oczekiwania — sprawdź hasło i spróbuj ponownie.");
                        return GLib.Source.REMOVE;
                    }
                    return GLib.Source.CONTINUE;
                });
                return GLib.Source.REMOVE;
            });
        });
    }

    // ══════════════════════════════════════════════════════════════════
    // Page 3: Locale + Keyboard
    // ══════════════════════════════════════════════════════════════════
    private Adw.NavigationPage build_locale_page () {
        var content = new Gtk.Box (Gtk.Orientation.VERTICAL, 20);
        content.append (oobe_heading (
            t ("Language & Keyboard", "Dil ve Klavye", "Język i klawiatura"),
            t ("Choose how KibaOS should communicate with you.",
               "KibaOS'un sizinle nasıl iletişim kuracağını seçin.",
               "Wybierz, w jaki sposób KibaOS ma się z Tobą komunikować.")));

        var locale_row = new Adw.ComboRow () { title = t ("Language", "Dil", "Język") };
        var locale_model = new Gtk.StringList (null);
        // Display names shown in the picker; `locales` holds the actual
        // locale strings the installed system will use, in the same order.
        string[] locale_labels = {
            "English (US)", "English (UK)", "Deutsch", "Français",
            "Español", "日本語", "中文（简体）", "Türkçe", "Polski"
        };
        string[] locales = {
            "en_US.UTF-8", "en_GB.UTF-8", "de_DE.UTF-8",
            "fr_FR.UTF-8", "es_ES.UTF-8", "ja_JP.UTF-8", "zh_CN.UTF-8",
            "tr_TR.UTF-8", "pl_PL.UTF-8"
        };
        foreach (var l in locale_labels) locale_model.append (l);
        locale_row.set_model (locale_model);
        locale_row.notify["selected"].connect (() => {
            selected_locale = locales[locale_row.get_selected ()];
        });
        content.append (locale_row);

        var keymap_row = new Adw.ComboRow () { title = t ("Keyboard layout", "Klavye düzeni", "Układ klawiatury") };
        var keymap_model = new Gtk.StringList (null);
        string[] keymap_labels = {
            "English (US)", "English (UK)", "Deutsch", "Français",
            "Español", "日本語", "Dvorak", "Türkçe (Q)", "Polski"
        };
        string[] keymaps = { "us", "uk", "de", "fr", "es", "jp106", "dvorak", "trq", "pl" };
        foreach (var k in keymap_labels) keymap_model.append (k);
        keymap_row.set_model (keymap_model);
        keymap_row.notify["selected"].connect (() => {
            selected_keymap = keymaps[keymap_row.get_selected ()];
        });
        content.append (keymap_row);

        return make_page ("Language", content, t ("Next", "İleri", "Dalej"), () => {
            if (is_oem_mode) nav_view.push (build_account_page ());
            else advance_past_storage_step ();
        }, false, 2, 6);
    }

    // ══════════════════════════════════════════════════════════════════
    // Storage detection helpers
    // ══════════════════════════════════════════════════════════════════
    private string boot_device_basename () {
        string s = "";
        try { GLib.Process.spawn_command_line_sync (
                "findmnt -n -o SOURCE /run/archiso/bootmnt", out s); }
        catch (GLib.SpawnError e) {}
        return strip_partition_suffix (GLib.Path.get_basename (s.strip ()));
    }

    // True if `devpath` already has a partition table with at least one
    // partition on it — our signal that there might be another OS here,
    // worth asking the user whether to erase it or install alongside it.
    private bool disk_has_existing_data (string devpath) {
        string raw = "";
        try { GLib.Process.spawn_command_line_sync (
                "lsblk -rno NAME %s".printf (devpath), out raw); }
        catch (GLib.SpawnError e) { return false; }
        int lines = 0;
        foreach (var line in raw.split ("\n")) {
            if (line.strip () != "") lines++;
        }
        // First line is the disk itself; anything beyond that means at
        // least one partition already exists.
        return lines > 1;
    }

    private class StorageOption {
        public string devpath;
        public string label;
    }

    private Gee.ArrayList<StorageOption> list_storage_options () {
        var options  = new Gee.ArrayList<StorageOption> ();
        string boot_dev = boot_device_basename ();
        string raw = "";
        try { GLib.Process.spawn_command_line_sync (
                "lsblk -dpno NAME,SIZE,MODEL,RM -e7,11", out raw); }
        catch (GLib.SpawnError e) { return options; }

        foreach (var line in raw.split ("\n")) {
            var trimmed = line.strip ();
            if (trimmed == "") continue;
            var parts   = trimmed.split (" ", 2);
            string devpath = parts[0];
            string rest    = parts.length > 1 ? parts[1].strip () : "";
            if (rest.has_suffix (" 1") || rest == "1") continue;
            if (GLib.Path.get_basename (devpath) == boot_dev) continue;
            string label = rest;
            try { label = /\s+[01]$/.replace (label, -1, 0, ""); }
            catch (GLib.RegexError e) {}
            var opt  = new StorageOption ();
            opt.devpath = devpath;
            opt.label   = t ("Your computer's storage (%s)", "Bilgisayarınızın deposu (%s)", "Pamięć Twojego komputera (%s)").printf (
                label == "" ? t ("internal drive", "dahili disk", "dysk wewnętrzny") : label);
            options.add (opt);
        }
        return options;
    }

    private void advance_past_storage_step () {
        var options = list_storage_options ();
        if (options.size <= 1) {
            selected_disk = options.size == 1 ? options[0].devpath : "";
            advance_past_install_mode_step ();
        } else {
            nav_view.push (build_storage_picker_page (options));
        }
    }

    // After a disk is settled on: if it looks like it already has an OS
    // on it, ask erase-vs-alongside; otherwise there's nothing to ask
    // (a blank disk can only be erased/initialized) so skip straight on.
    private void advance_past_install_mode_step () {
        install_mode = "erase";
        if (selected_disk != "" && disk_has_existing_data (selected_disk)) {
            nav_view.push (build_install_mode_page ());
        } else {
            nav_view.push (build_account_page ());
        }
    }

    // ══════════════════════════════════════════════════════════════════
    // Page 4: Storage picker (only shown with 2+ drives)
    // ══════════════════════════════════════════════════════════════════
    private Adw.NavigationPage build_storage_picker_page (Gee.ArrayList<StorageOption> options) {
        var content = new Gtk.Box (Gtk.Orientation.VERTICAL, 20);
        content.append (oobe_heading (
            t ("Where should KibaOS go?", "KibaOS nereye kurulsun?", "Gdzie zainstalować KibaOS?"),
            t ("Your computer has more than one drive. Pick the one to set up.",
               "Bilgisayarınızda birden fazla disk var. Kurulacak olanı seçin.",
               "Twój komputer ma więcej niż jeden dysk. Wybierz ten, na którym chcesz zainstalować system.")));

        var picker = new Gtk.ListBox ();
        picker.add_css_class ("oobe-list");
        picker.selection_mode = Gtk.SelectionMode.SINGLE;

        foreach (var opt in options) {
            var row = new Adw.ActionRow () { title = opt.label };
            row.set_data ("devpath", opt.devpath);
            picker.append (row);
        }
        picker.row_selected.connect ((row) => {
            if (row != null) selected_disk = row.get_data<string> ("devpath");
        });
        if (options.size > 0) {
            selected_disk = options[0].devpath;
            picker.select_row (picker.get_row_at_index (0));
        }
        content.append (picker);

        return make_page ("Storage", content, t ("Next", "İleri", "Dalej"), () => {
            advance_past_install_mode_step ();
        }, false, 3, 6);
    }

    // ══════════════════════════════════════════════════════════════════
    // Page 4b: Install mode (erase vs. install alongside) — only shown
    // when the chosen drive already has an existing OS/partition table
    // ══════════════════════════════════════════════════════════════════
    private Adw.NavigationPage build_install_mode_page () {
        var content = new Gtk.Box (Gtk.Orientation.VERTICAL, 20);
        content.append (oobe_heading (
            t ("How should KibaOS be installed?", "KibaOS nasıl kurulsun?", "Jak zainstalować KibaOS?"),
            t ("We found an existing operating system on this drive.",
               "Bu diskte mevcut bir işletim sistemi bulduk.",
               "Znaleźliśmy na tym dysku istniejący system operacyjny.")));

        var picker = new Gtk.ListBox ();
        picker.add_css_class ("oobe-list");
        picker.selection_mode = Gtk.SelectionMode.SINGLE;

        var erase_row = new Adw.ActionRow () {
            title    = t ("Erase disk", "Diski sil", "Wyczyść dysk"),
            subtitle = t ("Delete everything on this drive and install KibaOS by itself.",
                           "Bu diskteki her şeyi silip yalnızca KibaOS'u kurun.",
                           "Usuń wszystko z tego dysku i zainstaluj wyłącznie KibaOS.")
        };
        erase_row.set_data ("mode", "erase");
        picker.append (erase_row);

        var alongside_row = new Adw.ActionRow () {
            title    = t ("Install alongside", "Yanına kur", "Zainstaluj obok"),
            subtitle = t ("Keep what's already here and set up KibaOS in the free space next to it (dual boot).",
                           "Mevcut sistemi koru, KibaOS'u yanındaki boş alana kur (çift önyükleme).",
                           "Zachowaj to, co już tu jest, i zainstaluj KibaOS w wolnym miejscu obok (dual boot).")
        };
        alongside_row.set_data ("mode", "alongside");
        picker.append (alongside_row);

        picker.row_selected.connect ((row) => {
            if (row != null) install_mode = row.get_data<string> ("mode");
        });
        install_mode = "erase";
        picker.select_row (picker.get_row_at_index (0));
        content.append (picker);

        return make_page ("Install Mode", content, t ("Next", "İleri", "Dalej"), () => {
            nav_view.push (build_account_page ());
        }, false, 3, 6);
    }

    // ══════════════════════════════════════════════════════════════════
    // Page 5: Account creation
    // ══════════════════════════════════════════════════════════════════
    private Adw.NavigationPage build_account_page () {
        var content = new Gtk.Box (Gtk.Orientation.VERTICAL, 16);
        content.append (oobe_heading (
            t ("Create Your Account", "Hesabınızı Oluşturun", "Utwórz swoje konto"),
            t ("This is the account you'll use every day.",
               "Bu, her gün kullanacağınız hesap.",
               "To konto, którego będziesz używać na co dzień.")));

        var group = new Adw.PreferencesGroup ();
        group.add_css_class ("oobe-prefs-group");
        // "overflow: hidden" in oobe.css doesn't do anything -- GTK4's CSS
        // engine doesn't have that property, confirmed by the theme parser
        // warning it throws on startup. Clipping child rows to the
        // rounded-corner container is a widget property, not CSS.
        group.overflow = Gtk.Overflow.HIDDEN;

        var hostname_entry = new Adw.EntryRow () { title = t ("Computer name", "Bilgisayar adı", "Nazwa komputera") };
        hostname_entry.text = "kibaos";
        hostname_entry.changed.connect (() => { hostname_value = hostname_entry.text; });
        group.add (hostname_entry);

        var user_entry = new Adw.EntryRow () { title = t ("Username", "Kullanıcı adı", "Nazwa użytkownika") };
        user_entry.changed.connect (() => { username_value = user_entry.text; });
        group.add (user_entry);

        var pass_entry = new Adw.PasswordEntryRow () { title = t ("Password", "Şifre", "Hasło") };
        pass_entry.changed.connect (() => { password_value = pass_entry.text; });
        group.add (pass_entry);

        content.append (group);

        // No WinApps toggle here -- it's a listed, always-on feature (see
        // WINDOWS APP SUPPORT section), not opt-in. Both backends always
        // drop /etc/kibaos/winapps-pending unconditionally on finish.

        return make_page ("Account", content,
            is_oem_mode ? t ("Finish Setup", "Kurulumu Bitir", "Zakończ konfigurację") : t ("Next", "İleri", "Dalej"), () => {
                if (is_oem_mode) {
                    nav_view.push (build_installing_page ());
                    start_oem_finish ();
                } else {
                    nav_view.push (build_confirm_page ());
                }
            }, false, 4, 6);
    }

    // ══════════════════════════════════════════════════════════════════
    // Page 6: Confirm
    // ══════════════════════════════════════════════════════════════════
    private Adw.NavigationPage build_confirm_page () {
        var content = new Gtk.Box (Gtk.Orientation.VERTICAL, 20);
        content.append (oobe_heading (
            t ("Ready to Set Up KibaOS", "KibaOS Kurulumuna Hazır", "Gotowy do instalacji KibaOS"),
            install_mode == "alongside"
                ? t ("KibaOS will be installed next to your existing operating system, " +
                     "using the free space on this drive. Nothing else will be touched.",
                     "KibaOS, mevcut işletim sisteminizin yanına, bu diskteki boş alan " +
                     "kullanılarak kurulacak. Başka hiçbir şeye dokunulmayacak.",
                     "KibaOS zostanie zainstalowany obok Twojego obecnego systemu operacyjnego, wykorzystując wolne miejsce na tym dysku. Nic innego nie zostanie zmienione.")
                : t ("Everything on your computer will be replaced. " +
                     "Make sure anything important is backed up first.",
                     "Bilgisayarınızdaki her şeyin yerine yenisi kurulacak. " +
                     "Önemli olan her şeyi önceden yedeklediğinizden emin olun.",
                     "Wszystko na Twoim komputerze zostanie zastąpione. Upewnij się wcześniej, że wszystkie ważne dane masz zapisane w kopii zapasowej.")));

        // Summary card
        var summary = new Gtk.Box (Gtk.Orientation.VERTICAL, 0);
        summary.add_css_class ("oobe-summary-box");
        summary.overflow = Gtk.Overflow.HIDDEN;

        OobeSummaryItem[] items = {
            { t ("Storage", "Depolama", "Pamięć"),
              selected_disk == "" ? t ("Auto-detected", "Otomatik algılandı", "Wykryto automatycznie") : GLib.Path.get_basename (selected_disk) },
            { t ("Install mode", "Kurulum modu", "Tryb instalacji"),
              install_mode == "alongside" ? t ("Install alongside (dual boot)", "Yanına kur (çift önyükleme)", "Zainstaluj obok (dual boot)") : t ("Erase disk", "Diski sil", "Wyczyść dysk") },
            { t ("Language", "Dil", "Język"), selected_locale },
            { t ("Keyboard", "Klavye", "Klawiatura"), selected_keymap },
            { t ("Account", "Hesap", "Konto"),
              username_value == "" ? t ("(not set)", "(ayarlanmadı)", "(nie ustawiono)") : username_value }
        };
        bool first = true;
        foreach (var item in items) {
            if (!first) {
                var sep = new Gtk.Separator (Gtk.Orientation.HORIZONTAL);
                sep.add_css_class ("oobe-summary-sep");
                summary.append (sep);
            }
            first = false;
            var row = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 12);
            row.add_css_class ("oobe-summary-row");
            var lbl = new Gtk.Label (item.key);
            lbl.add_css_class ("oobe-summary-key");
            row.append (lbl);
            var spacer = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 0) { hexpand = true };
            row.append (spacer);
            var val = new Gtk.Label (item.val);
            val.add_css_class ("oobe-summary-val");
            row.append (val);
            summary.append (row);
        }
        content.append (summary);

        return make_page ("Confirm", content, t ("Install KibaOS", "KibaOS'u Kur", "Zainstaluj KibaOS"), () => {
            nav_view.push (build_installing_page ());
            start_install ();
        }, false, 5, 6);
    }

    // ══════════════════════════════════════════════════════════════════
    // Page 7: Installing
    // ══════════════════════════════════════════════════════════════════
    private Gtk.Label      progress_label;
    private Gtk.ProgressBar progress_bar;
    private Gtk.Stack      feature_stack;
    private uint            feature_timer_id = 0;

    // ── Install-page dock animation ─────────────────────────────────────
    // Ported from the KibaOS Desktop web loading animation: a glass
    // taskbar with app icons and a small virtual cursor that glides
    // across it, "clicking" a couple of icons along the way. The web
    // version was a fixed 15s intro; here the install duration is
    // unknown ahead of time, so the same 15s choreography just loops
    // for as long as the backend takes.
    private const int64  DOCK_DURATION_MS = 15000;
    private const double DOCK_SPACING     = 66.0;
    private const double DOCK_START_X     = 10.0;
    private const double DOCK_ICON_Y      = 10.0;
    private const double DOCK_ICON_HALF   = 26.0;

    private Gtk.Fixed     dock_taskbar;
    private Gtk.Widget[]  dock_icon_frames;
    private double[]      dock_icon_lift;
    private Gtk.Widget    dock_cursor;
    private int64         dock_anim_start_us = 0;
    private int64         dock_loop_index    = -1;
    private bool          dock_click1_done   = false;
    private bool          dock_click2_done   = false;
    private uint          dock_tick_id       = 0;

    private Gtk.Widget make_dock_icon (string icon_name, string tooltip, bool dot_active) {
        var frame = new Gtk.Box (Gtk.Orientation.VERTICAL, 5) {
            halign = Gtk.Align.CENTER,
            valign = Gtk.Align.START,
            width_request = 52
        };
        var icon_box = new Gtk.Box (Gtk.Orientation.VERTICAL, 0) {
            width_request = 52, height_request = 52,
            halign = Gtk.Align.CENTER, valign = Gtk.Align.CENTER
        };
        icon_box.add_css_class ("oobe-dock-icon");
        var img = new Gtk.Image.from_icon_name (icon_name) { pixel_size = 20 };
        icon_box.append (img);
        frame.append (icon_box);

        var dot = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 0) {
            halign = Gtk.Align.CENTER, width_request = 4, height_request = 4
        };
        dot.add_css_class ("oobe-dock-dot");
        if (dot_active) dot.add_css_class ("oobe-dock-dot-active");
        frame.append (dot);

        frame.tooltip_text = tooltip;
        // Stash the inner icon box so the tick callback can toggle its
        // "popped"/"adjacent" CSS class without walking the widget tree.
        frame.set_data<Gtk.Widget> ("icon-box", icon_box);
        return frame;
    }

    private Gtk.Widget build_install_dock () {
        string[,] apps = {
            // icon name,                          tooltip,                                                              starts with an "active" dot
            { "view-grid-symbolic",              t ("App Launcher", "Uygulama Başlatıcı", "Uruchamianie aplikacji"),   "1" },
            { "folder-symbolic",                  t ("File Manager", "Dosya Yöneticisi", "Menedżer plików"),           "0" },
            { "web-browser-symbolic",             t ("Web Browser", "Web Tarayıcı", "Przeglądarka"),                    "1" },
            { "utilities-terminal-symbolic",      t ("Terminal", "Uçbirim", "Terminal"),                                "0" },
            { "computer-symbolic",                t ("Windows (LSW)", "Windows (LSW)", "Windows (LSW)"),                "0" },
            { "system-software-install-symbolic", t ("kibapkg Store", "kibapkg Mağazası", "Sklep kibapkg"),            "0" },
            { "preferences-system-symbolic",      t ("Control Center", "Denetim Merkezi", "Centrum sterowania"),       "0" }
        };
        int n = apps.length[0];

        dock_taskbar = new Gtk.Fixed ();
        dock_taskbar.add_css_class ("oobe-dock-taskbar");
        dock_taskbar.width_request  = (int) (DOCK_START_X * 2 + DOCK_SPACING * (n - 1) + DOCK_ICON_HALF * 2);
        dock_taskbar.height_request = 132;

        dock_icon_frames = new Gtk.Widget[n];
        dock_icon_lift   = new double[n];
        for (int i = 0; i < n; i++) {
            var frame = make_dock_icon (apps[i, 0], apps[i, 1], apps[i, 2] == "1");
            dock_icon_frames[i] = frame;
            dock_taskbar.put (frame, DOCK_START_X + i * DOCK_SPACING, DOCK_ICON_Y);
        }

        dock_cursor = new Gtk.Image.from_icon_name ("pan-end-symbolic") { pixel_size = 16 };
        dock_cursor.add_css_class ("oobe-dock-cursor");
        dock_taskbar.put (dock_cursor, DOCK_START_X, DOCK_ICON_Y + 90);

        var wrap = new Gtk.Box (Gtk.Orientation.VERTICAL, 0) {
            halign = Gtk.Align.CENTER, margin_top = 4, margin_bottom = 4
        };
        wrap.append (dock_taskbar);

        dock_anim_start_us = 0;
        dock_loop_index    = -1;
        dock_click1_done   = false;
        dock_click2_done   = false;
        dock_tick_id = dock_taskbar.add_tick_callback (on_dock_tick);

        return wrap;
    }

    // Brief highlight on the cursor to stand in for the web version's
    // expanding click-ripple, without needing a second drawing surface.
    private void pulse_dock_cursor () {
        if (dock_cursor == null) return;
        dock_cursor.add_css_class ("oobe-dock-cursor-click");
        GLib.Timeout.add (260, () => {
            if (dock_cursor != null) dock_cursor.remove_css_class ("oobe-dock-cursor-click");
            return GLib.Source.REMOVE;
        });
    }

    // Same six-stage timeline as the original JS (swoop in -> glide across
    // all icons -> hover+click the last one -> glide back -> hover+click
    // App 5 (Windows/LSW) -> settle), just re-driven off GTK's frame clock
    // instead of performance.now(), and wrapped to repeat every 15s.
    private bool on_dock_tick (Gtk.Widget widget, Gdk.FrameClock frame_clock) {
        var taskbar = widget as Gtk.Fixed;
        if (taskbar == null || dock_icon_frames == null || dock_cursor == null) {
            return GLib.Source.REMOVE;
        }

        int64 now_us = frame_clock.get_frame_time ();
        if (dock_anim_start_us == 0) dock_anim_start_us = now_us;
        int64 elapsed_ms = (now_us - dock_anim_start_us) / 1000;

        int64 loop_idx = elapsed_ms / DOCK_DURATION_MS;
        if (loop_idx != dock_loop_index) {
            dock_loop_index  = loop_idx;
            dock_click1_done = false;
            dock_click2_done = false;
        }
        double t_ms = (double) (elapsed_ms % DOCK_DURATION_MS);

        int n = dock_icon_frames.length;
        double[] centers_x = new double[n];
        for (int i = 0; i < n; i++) centers_x[i] = DOCK_START_X + i * DOCK_SPACING + DOCK_ICON_HALF;
        double base_y = DOCK_ICON_Y + DOCK_ICON_HALF;

        double start_cx = centers_x[0] - 45;
        double start_cy = base_y + 60;
        int last_idx = n - 1;
        int lsw_idx  = int.min (4, last_idx);

        double cur_x, cur_y;
        if (t_ms < 800) {
            double e = t_ms / 800.0;
            double ease = 1 - Math.pow (1 - e, 3);
            cur_x = start_cx + (centers_x[0] - start_cx) * ease;
            cur_y = start_cy + (base_y - start_cy) * ease;
        } else if (t_ms < 7600) {
            double glide_t = (t_ms - 800) / 6800.0;
            double target_index = glide_t * last_idx;
            int i0 = (int) Math.floor (target_index);
            int i1 = int.min (i0 + 1, last_idx);
            double local_t = target_index - i0;
            double smooth_t = local_t * local_t * (3 - 2 * local_t);
            cur_x = centers_x[i0] + (centers_x[i1] - centers_x[i0]) * smooth_t;
            cur_y = base_y + Math.sin (glide_t * Math.PI * last_idx) * 3.5;
        } else if (t_ms < 9200) {
            cur_x = centers_x[last_idx];
            cur_y = base_y;
            if (t_ms >= 8200 && !dock_click1_done) {
                dock_click1_done = true;
                pulse_dock_cursor ();
            }
        } else if (t_ms < 12800) {
            double back_t = (t_ms - 9200) / 3600.0;
            double target_index = last_idx - back_t * (last_idx - lsw_idx);
            int i0 = (int) Math.ceil (target_index);
            int i1 = int.max (lsw_idx, (int) Math.floor (target_index));
            double local_t = i0 - target_index;
            double smooth_t = local_t * local_t * (3 - 2 * local_t);
            cur_x = centers_x[i0] + (centers_x[i1] - centers_x[i0]) * smooth_t;
            cur_y = base_y + Math.sin (back_t * Math.PI * 2) * 3;
        } else if (t_ms < 14400) {
            cur_x = centers_x[lsw_idx];
            cur_y = base_y;
            if (t_ms >= 13400 && !dock_click2_done) {
                dock_click2_done = true;
                pulse_dock_cursor ();
            }
        } else {
            double final_t = (t_ms - 14400) / 600.0;
            cur_x = centers_x[lsw_idx];
            cur_y = base_y + final_t * 8;
        }

        taskbar.move (dock_cursor, cur_x - 8, cur_y - 8);

        // Proximity-based pop, eased toward its target each frame so the
        // icons ease in/out instead of snapping (mirrors the web
        // version's is-popped / is-adjacent hover states + transition).
        int closest = 0;
        double min_dist = double.MAX;
        for (int i = 0; i < n; i++) {
            double dist = Math.fabs (cur_x - centers_x[i]);
            if (dist < min_dist) { min_dist = dist; closest = i; }
        }
        for (int i = 0; i < n; i++) {
            int diff = i - closest;
            if (diff < 0) diff = -diff;
            bool popped   = (i == closest && min_dist < 38);
            bool adjacent = (!popped && min_dist < 64 && diff == 1);

            double target_lift = popped ? 16.0 : (adjacent ? 6.0 : 0.0);
            dock_icon_lift[i] += (target_lift - dock_icon_lift[i]) * 0.25;
            taskbar.move (dock_icon_frames[i],
                          DOCK_START_X + i * DOCK_SPACING,
                          DOCK_ICON_Y - dock_icon_lift[i]);

            var icon_box = dock_icon_frames[i].get_data<Gtk.Widget> ("icon-box");
            if (icon_box != null) {
                icon_box.remove_css_class ("oobe-dock-icon-popped");
                icon_box.remove_css_class ("oobe-dock-icon-adjacent");
                if (popped)   icon_box.add_css_class ("oobe-dock-icon-popped");
                if (adjacent) icon_box.add_css_class ("oobe-dock-icon-adjacent");
            }
        }

        return GLib.Source.CONTINUE;
    }

    private void stop_dock_animation () {
        if (dock_tick_id != 0 && dock_taskbar != null) {
            dock_taskbar.remove_tick_callback (dock_tick_id);
        }
        dock_tick_id = 0;
    }

    private Adw.NavigationPage build_installing_page () {
        var content = new Gtk.Box (Gtk.Orientation.VERTICAL, 20);
        content.append (oobe_heading (
            t ("Installing KibaOS", "KibaOS Kuruluyor", "Instalowanie KibaOS"),
            t ("Sit tight — this won't take long.", "Biraz bekleyin — çok sürmeyecek.", "Chwila cierpliwości — to nie potrwa długo.")));

        // Taskbar animation, ported from the KibaOS web loading intro --
        // something more engaging than a static wait, which is exactly
        // what research on long-running progress UX recommends: occupied
        // waiting reads as shorter than unoccupied waiting.
        content.append (build_install_dock ());

        // show_text: research on progress indicators is consistent that a
        // determinate bar should pair its fill with a visible percentage,
        // not just a status string -- percentage answers "how much is
        // left", status answers "what's happening", and users want both.
        progress_bar = new Gtk.ProgressBar () { show_text = true };
        progress_bar.add_css_class ("oobe-progress");
        content.append (progress_bar);

        progress_label = new Gtk.Label (t ("Preparing…", "Hazırlanıyor…", "Przygotowywanie…"));
        progress_label.add_css_class ("oobe-subtitle");
        content.append (progress_label);

        // Feature slideshow -- something to actually read while the real
        // progress line above is stuck on one status for a while (e.g.
        // "Copying files…" during the unsquashfs step, which takes a lot
        // longer than everything else combined and gives no finer-grained
        // updates). Cycles independently of install progress; the two
        // labels aren't tied together.
        string[,] features = {
            { t ("Your computer learns how you use it", "Bilgisayarınız sizi nasıl kullandığınızı öğrenir", "Twój komputer uczy się, jak go używasz"),
              t ("KibaOS quietly notices your habits and adjusts things to fit — everything stays on your computer.",
                 "KibaOS alışkanlıklarınızı sessizce fark eder ve buna göre ayarlar yapar — her şey bilgisayarınızda kalır.",
                 "KibaOS po cichu zauważa Twoje nawyki i odpowiednio się dostosowuje — wszystko zostaje na Twoim komputerze.") },
            { t ("Built just for this installer", "Sadece bu kurulum için yapıldı", "Zbudowany specjalnie dla tego instalatora"),
              t ("This setup screen was made specifically for KibaOS, not borrowed from another system.",
                 "Bu kurulum ekranı özellikle KibaOS için yapıldı, başka bir sistemden alınmadı.",
                 "Ten ekran instalacji został stworzony specjalnie dla KibaOS, a nie zapożyczony z innego systemu.") },
            { t ("Starts up fast", "Hızlı açılır", "Szybko się uruchamia"),
              t ("KibaOS uses your computer's modern startup process, which gets you to the desktop quicker.",
                 "KibaOS, bilgisayarınızın modern açılış sürecini kullanır ve bu sayede masaüstüne daha hızlı ulaşırsınız.",
                 "KibaOS korzysta z nowoczesnego procesu uruchamiania komputera, dzięki czemu szybciej trafiasz na pulpit.") },
            { t ("Fixes small problems on its own", "Küçük sorunları kendi kendine çözer", "Samodzielnie naprawia drobne problemy"),
              t ("If something starts acting up, KibaOS notices and tries to repair it automatically.",
                 "Bir şey tuhaf davranmaya başlarsa, KibaOS bunu fark eder ve otomatik olarak onarmaya çalışır.",
                 "Jeśli coś zacznie działać nieprawidłowo, KibaOS to zauważy i spróbuje to automatycznie naprawić.") },
            { t ("Ready to play your videos and music", "Videolarınızı ve müziklerinizi oynatmaya hazır", "Gotowy do odtwarzania Twoich filmów i muzyki"),
              t ("A video player and everything it needs are already installed — nothing extra to download.",
                 "Bir video oynatıcı ve ihtiyaç duyduğu her şey zaten kurulu — indirmeniz gereken ekstra bir şey yok.",
                 "Odtwarzacz wideo i wszystko, czego potrzebuje, jest już zainstalowane — nie trzeba niczego dodatkowo pobierać.") }
        };

        feature_stack = new Gtk.Stack () {
            transition_type = Gtk.StackTransitionType.CROSSFADE,
            transition_duration = 420,
            margin_top = 40,
            halign = Gtk.Align.CENTER
        };
        feature_stack.add_css_class ("oobe-feature-stack");
        for (int i = 0; i < features.length[0]; i++) {
            var pane = new Gtk.Box (Gtk.Orientation.VERTICAL, 6) { halign = Gtk.Align.CENTER };
            var f_title = new Gtk.Label (features[i, 0]) { halign = Gtk.Align.CENTER, justify = Gtk.Justification.CENTER };
            f_title.add_css_class ("oobe-feature-title");
            var f_body = new Gtk.Label (features[i, 1]) {
                halign = Gtk.Align.CENTER, justify = Gtk.Justification.CENTER, wrap = true, max_width_chars = 46
            };
            f_body.add_css_class ("oobe-feature-body");
            pane.append (f_title);
            pane.append (f_body);
            feature_stack.add_named (pane, i.to_string ());
        }
        feature_stack.visible_child_name = "0";
        content.append (feature_stack);

        int feature_index = 0;
        int feature_count = features.length[0];
        feature_timer_id = GLib.Timeout.add_seconds (5, () => {
            feature_index = (feature_index + 1) % feature_count;
            feature_stack.visible_child_name = feature_index.to_string ();
            return GLib.Source.CONTINUE;
        });

        return make_page ("Installing", content, null, null, true);
    }

    // Stops the feature-slideshow timer -- called once install finishes
    // (success or failure) so it doesn't keep firing against a stack that
    // no longer has a reason to update once this page is behind us.
    private void stop_feature_slideshow () {
        if (feature_timer_id != 0) {
            GLib.Source.remove (feature_timer_id);
            feature_timer_id = 0;
        }
    }

    // ══════════════════════════════════════════════════════════════════
    // Page 8: Done
    // ══════════════════════════════════════════════════════════════════
    private Adw.NavigationPage build_done_page () {
        var content = new Gtk.Box (Gtk.Orientation.VERTICAL, 20);

        var check = new Gtk.Label ("✓") {
            halign = Gtk.Align.START
        };
        check.add_css_class ("oobe-done-check");
        content.append (check);

        content.append (oobe_heading (
            t ("You're all set.", "Her şey hazır.", "Wszystko gotowe."),
            t ("KibaOS is installed and ready. Restart your computer to get started.",
               "KibaOS kuruldu ve hazır. Başlamak için bilgisayarınızı yeniden başlatın.",
               "KibaOS został zainstalowany i jest gotowy. Uruchom ponownie komputer, aby rozpocząć.")));

        return make_page ("Done", content, t ("Restart Now", "Şimdi Yeniden Başlat", "Uruchom ponownie teraz"), () => {
            try { GLib.Process.spawn_command_line_async ("systemctl reboot"); }
            catch (GLib.SpawnError e) { warning ("Reboot failed: %s", e.message); }
        }, true);
    }

    // ══════════════════════════════════════════════════════════════════
    // Backend plumbing
    // ══════════════════════════════════════════════════════════════════
    private void start_oem_finish () {
        string[] argv = {
            "sudo", "/usr/local/bin/kibaos-oem-finish.sh",
            selected_locale, selected_keymap, hostname_value,
            username_value, password_value
        };
        launch_backend (argv);
    }

    private void start_install () {
        // Disk partitioning/formatting, base extraction, and bootloader
        // install all go through libkibadisk (sgdisk-subprocess-backed GPT
        // writer) in kibaos-oobe-backend directly -- no archinstall detour.
        string[] argv = {
            "sudo", "/usr/local/bin/kibaos-oobe-backend",
            selected_disk, install_mode, selected_locale, selected_keymap,
            hostname_value, username_value, password_value
        };
        launch_backend (argv);
    }

    private string last_fatal_message = "";

    private void launch_backend (string[] argv) {
        try {
            // STDERR_MERGE folds the backend's stderr into the same pipe as
            // stdout. Previously only STDOUT_PIPE was set, so every
            // "FATAL: ..." line (the only place the *actual* error reason
            // — kiba_fs_strerror()/strerror(errno) text — ever got written)
            // went to the backend's inherited stderr and was simply lost,
            // since sudo+a GUI launch has no terminal attached to catch it.
            var launcher = new GLib.SubprocessLauncher (
                GLib.SubprocessFlags.STDOUT_PIPE | GLib.SubprocessFlags.STDERR_MERGE);
            var proc     = launcher.spawnv (argv);
            last_fatal_message = "";
            read_backend_output.begin (
                new GLib.DataInputStream (proc.get_stdout_pipe ()), proc);
        } catch (GLib.Error e) {
            progress_label.label = t ("Failed to start: %s", "Başlatılamadı: %s", "Nie udało się uruchomić: %s").printf (e.message);
        }
    }

    private async void read_backend_output (GLib.DataInputStream stream, GLib.Subprocess proc) {
        try {
            while (true) {
                string? line = yield stream.read_line_async ();
                if (line == null) break;
                if (line.has_prefix ("PROGRESS ")) {
                    var parts = line.substring (9).split (" ", 2);
                    int    pct = int.parse (parts[0]);
                    string msg = parts.length > 1 ? parts[1] : "";
                    progress_bar.fraction = pct / 100.0;
                    progress_bar.text     = "%d%%".printf (pct);
                    progress_label.label  = msg;
                } else if (line.has_prefix ("FATAL: ")) {
                    // Captured now that stderr is merged in — keep the real
                    // reason so we can show it instead of a generic message.
                    last_fatal_message = line.substring (7);
                }
            }
            yield proc.wait_async ();
            stop_feature_slideshow ();
            stop_dock_animation ();
            if (proc.get_exit_status () == 0) {
                nav_view.push (build_done_page ());
            } else if (last_fatal_message != "") {
                progress_label.label = last_fatal_message +
                    t ("\n(Full log: /var/log/kibaos-oobe.log)",
                       "\n(Tam günlük: /var/log/kibaos-oobe.log)",
                       "\\n(Pełny dziennik: /var/log/kibaos-oobe.log)");
            } else {
                progress_label.label = t (
                    "Something went wrong. Check /var/log/kibaos-oobe.log for details.",
                    "Bir şeyler ters gitti. Ayrıntılar için /var/log/kibaos-oobe.log dosyasına bakın.",
                    "Coś poszło nie tak. Szczegóły znajdziesz w /var/log/kibaos-oobe.log.");
            }
        } catch (GLib.Error e) {
            stop_feature_slideshow ();
            stop_dock_animation ();
            progress_label.label = t ("Lost connection to installer: %s",
                                       "Kurulum programıyla bağlantı kesildi: %s",
                                       "Utracono połączenie z instalatorem: %s").printf (e.message);
        }
    }

    public static int main (string[] args) {
        return new KibaOOBE ().run (args);
    }
}
