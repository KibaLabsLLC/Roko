public class KibaWinAppsSetup : Adw.Application {
    private Adw.ApplicationWindow window;
    private Gtk.ProgressBar progress_bar;
    private Gtk.Label       heading_label;
    private Gtk.Label       status_label;
    private Gtk.Box         button_row;
    private Gtk.Button      retry_btn;
    private Gtk.Button      open_btn;
    private Gtk.Button      close_btn;
    private string          last_fatal_message = "";
    private string[]        launch_args;

    // Same tiny inline translator as KibaOOBE (see main.vala) -- kept as
    // a separate copy rather than a shared header, since this is a
    // single-file build target and Vala has no lightweight way to share
    // one private method across two unrelated executable() targets
    // without a proper library split, which is more plumbing than a
    // three-line helper is worth here.
    private string ui_lang = "en";
    private string t (string en, string tr, string pl) {
        return ui_lang == "tr" ? tr : ui_lang == "pl" ? pl : en;
    }

    public KibaWinAppsSetup (string[] args) {
        Object (application_id: "io.kibaos.winapps-setup", flags: ApplicationFlags.FLAGS_NONE);
        // Everything after argv[0] -- just the optional "--manual-launch"
        // flag kibaos-winapps-workspace already passes today -- gets
        // forwarded straight through to the backend unchanged, same as
        // it always did back when kibaos-winapps-workspace exec'd the
        // backend directly.
        launch_args = args;
    }

    protected override void activate () {
        var locale = GLib.Environment.get_variable ("LANG") ?? "";
        if (locale.has_prefix ("tr")) ui_lang = "tr";
        else if (locale.has_prefix ("pl")) ui_lang = "pl";

        window = new Adw.ApplicationWindow (this) {
            default_width  = 480,
            default_height = 420,
            resizable      = false,
            title = t ("Windows Workspace Setup",
                       "Windows Çalışma Alanı Kurulumu",
                       "Konfiguracja Windows Workspace")
        };

        var toolbar = new Adw.ToolbarView ();
        toolbar.add_top_bar (new Adw.HeaderBar ());

        var content = new Gtk.Box (Gtk.Orientation.VERTICAL, 18) {
            halign = Gtk.Align.CENTER, valign = Gtk.Align.CENTER,
            margin_top = 12, margin_bottom = 30, margin_start = 36, margin_end = 36
        };

        var icon = new Gtk.Image.from_icon_name ("kibaos-winapps") {
            pixel_size = 64, halign = Gtk.Align.CENTER
        };
        content.append (icon);

        heading_label = new Gtk.Label (
            t ("Setting up Windows Workspace",
               "Windows Çalışma Alanı Kuruluyor",
               "Konfigurowanie Windows Workspace")) {
            halign = Gtk.Align.CENTER, justify = Gtk.Justification.CENTER
        };
        heading_label.add_css_class ("title-1");
        content.append (heading_label);

        progress_bar = new Gtk.ProgressBar () { show_text = false, hexpand = true };
        content.append (progress_bar);

        status_label = new Gtk.Label (t ("Starting…", "Başlatılıyor…", "Uruchamianie…")) {
            halign = Gtk.Align.CENTER, justify = Gtk.Justification.CENTER,
            wrap = true, max_width_chars = 48
        };
        status_label.add_css_class ("dim-label");
        content.append (status_label);

        // All three buttons exist from the start, just hidden -- toggling
        // visibility rather than reparenting widgets keeps run_backend()
        // (which is also the retry path) simple to reset between runs.
        button_row = new Gtk.Box (Gtk.Orientation.HORIZONTAL, 10) {
            halign = Gtk.Align.CENTER, visible = false
        };
        retry_btn = new Gtk.Button.with_label (t ("Retry", "Tekrar Dene", "Spróbuj ponownie"));
        retry_btn.add_css_class ("suggested-action");
        retry_btn.clicked.connect (() => run_backend ());
        open_btn = new Gtk.Button.with_label (
            t ("Open Windows Workspace", "Windows Çalışma Alanını Aç", "Otwórz Windows Workspace"));
        open_btn.add_css_class ("suggested-action");
        open_btn.clicked.connect (() => {
            try { GLib.Process.spawn_command_line_async ("/usr/local/bin/kibaos-winapps-workspace"); }
            catch (GLib.SpawnError e) { warning ("Failed to launch Windows Workspace: %s", e.message); }
            window.close ();
        });
        close_btn = new Gtk.Button.with_label (t ("Close", "Kapat", "Zamknij"));
        close_btn.add_css_class ("flat");
        close_btn.clicked.connect (() => window.close ());
        button_row.append (retry_btn);
        button_row.append (open_btn);
        button_row.append (close_btn);
        content.append (button_row);

        toolbar.set_content (content);
        window.set_content (toolbar);
        window.present ();

        run_backend ();
    }

    // ══════════════════════════════════════════════════════════════════
    // Backend plumbing -- same PROGRESS/FATAL reader as KibaOOBE's
    // launch_backend()/read_backend_output() (see main.vala), just
    // pointed at kibaos-winapps-setup instead of kibaos-oobe-backend/
    // kibaos-oem-finish.sh, and spawned WITHOUT a sudo/pkexec prefix --
    // unlike those two, this backend has to run as the actual invoking
    // user (it writes under $HOME/.config/winapps) and elevates only the
    // specific docker/systemd calls it needs, itself, inline, via its
    // own pkexec calls. Wrapping the whole thing in sudo here would hand
    // it root's $HOME instead and break that.
    // ══════════════════════════════════════════════════════════════════
    private void run_backend () {
        button_row.visible = false;
        retry_btn.visible  = false;
        open_btn.visible   = false;
        close_btn.visible  = false;
        heading_label.label = t ("Setting up Windows Workspace",
                                  "Windows Çalışma Alanı Kuruluyor",
                                  "Konfigurowanie Windows Workspace");
        progress_bar.fraction = 0.0;
        // Undo whatever show_failure_state() below left behind from a
        // previous failed attempt -- without this, a retry that succeeds
        // would still show the status line in error styling.
        status_label.remove_css_class ("error");
        status_label.add_css_class ("dim-label");
        status_label.label = t ("Starting…", "Başlatılıyor…", "Uruchamianie…");

        string[] argv = (launch_args.length > 1)
            ? new string[] { "/usr/local/bin/kibaos-winapps-setup", launch_args[1] }
            : new string[] { "/usr/local/bin/kibaos-winapps-setup" };

        try {
            var launcher = new GLib.SubprocessLauncher (
                GLib.SubprocessFlags.STDOUT_PIPE | GLib.SubprocessFlags.STDERR_MERGE);
            var proc = launcher.spawnv (argv);
            last_fatal_message = "";
            read_backend_output.begin (
                new GLib.DataInputStream (proc.get_stdout_pipe ()), proc);
        } catch (GLib.Error e) {
            status_label.label = t ("Failed to start: %s", "Başlatılamadı: %s",
                                     "Nie udało się uruchomić: %s").printf (e.message);
            show_failure_state ();
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
                    status_label.label    = msg;
                } else if (line.has_prefix ("FATAL: ")) {
                    // Same reasoning as KibaOOBE: STDERR_MERGE means this
                    // is the one place the real failure reason (not a
                    // generic message) is ever actually available.
                    last_fatal_message = line.substring (7);
                }
            }
            yield proc.wait_async ();
            if (proc.get_exit_status () == 0) {
                heading_label.label   = t ("All set!", "Her şey hazır!", "Wszystko gotowe!");
                progress_bar.fraction = 1.0;
                open_btn.visible      = true;
                close_btn.visible     = true;
                button_row.visible    = true;
            } else {
                heading_label.label = t ("Setup didn't finish", "Kurulum tamamlanamadı",
                                          "Konfiguracja się nie powiodła");
                status_label.label = last_fatal_message != "" ? last_fatal_message : t (
                    "Something went wrong. Check the system log (journalctl -t kibaos-winapps-setup) for details.",
                    "Bir şeyler ters gitti. Ayrıntılar için sistem günlüğünü kontrol edin (journalctl -t kibaos-winapps-setup).",
                    "Coś poszło nie tak. Sprawdź dziennik systemowy (journalctl -t kibaos-winapps-setup), aby uzyskać szczegóły.");
                show_failure_state ();
            }
        } catch (GLib.Error e) {
            heading_label.label = t ("Setup didn't finish", "Kurulum tamamlanamadı",
                                      "Konfiguracja się nie powiodła");
            status_label.label = t ("Lost connection to the setup process: %s",
                                     "Kurulum sürecine bağlantı kesildi: %s",
                                     "Utracono połączenie z procesem konfiguracji: %s").printf (e.message);
            show_failure_state ();
        }
    }

    private void show_failure_state () {
        status_label.remove_css_class ("dim-label");
        status_label.add_css_class ("error");
        retry_btn.visible  = true;
        close_btn.visible  = true;
        button_row.visible = true;
    }

    public static int main (string[] args) {
        return new KibaWinAppsSetup (args).run (args);
    }
}
