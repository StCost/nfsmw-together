// NFS MW Together launcher: edits scripts\MWTogether.ini (comments kept) and starts speed.exe.
// Lives next to speed.exe. Build (csc from .NET Framework 4, nothing to install):
//   csc /target:winexe /out:MWTogetherLauncher.exe /r:System.Windows.Forms.dll /r:System.Drawing.dll
//       /resource:docs\banner.png,banner.png launcher\Launcher.cs
using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

static class Ini {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern int GetPrivateProfileString(string s, string k, string d, StringBuilder r, int n, string f);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern bool WritePrivateProfileString(string s, string k, string v, string f);
    public static string Path;
    public static string Get(string s, string k, string d) { var sb = new StringBuilder(512); GetPrivateProfileString(s, k, d, sb, 512, Path); return sb.ToString(); }
    public static bool GetB(string s, string k, bool d) { return Get(s, k, d ? "1" : "0").Trim() != "0"; }
    public static void Set(string s, string k, string v) { WritePrivateProfileString(s, k, v, Path); }
    public static void SetB(string s, string k, bool v) { Set(s, k, v ? "1" : "0"); }
}

// Most Wanted look (taken from the game's main menu): lowercase heavy serif headings, bold white
// labels, dark panels with diagonal hatch stripes, amber corner brackets on the selected item, amber
// chevrons.
class GameButton : Button {
    public bool Selected, Plain, Chevrons; public int Arrow;   // Arrow: 1 = points right (closed), 2 = points down (open)
    bool hover;
    public GameButton() { SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true); }
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnPaint(PaintEventArgs e) {
        var g = e.Graphics; var r = ClientRectangle;
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        Theme.PaintBg(g, this);
        if (!Plain) {
            Theme.Stripes(g, r, hover ? Color.FromArgb(86, 80, 60) : Selected ? Color.FromArgb(64, 62, 56) : Color.FromArgb(40, 40, 38));
            if (hover && !Selected) using (var hp = new Pen(Color.FromArgb(150, Theme.Accent), 1)) g.DrawRectangle(hp, 0, 0, r.Width - 1, r.Height - 1);
            if (Selected) {   // amber corner brackets, like the game's selection marker
                using (var p = new Pen(Theme.Accent, 3)) {
                    int L = Math.Min(14, r.Height / 2), x0 = 1, y0 = 1, x1 = r.Width - 2, y1 = r.Height - 2;
                    g.DrawLines(p, new[] { new Point(x0, y0 + L), new Point(x0, y0), new Point(x0 + L, y0) });
                    g.DrawLines(p, new[] { new Point(x1 - L, y0), new Point(x1, y0), new Point(x1, y0 + L) });
                    g.DrawLines(p, new[] { new Point(x0, y1 - L), new Point(x0, y1), new Point(x0 + L, y1) });
                    g.DrawLines(p, new[] { new Point(x1 - L, y1), new Point(x1, y1), new Point(x1, y1 - L) });
                }
            }
        }
        Color fg = Plain ? (hover ? Theme.Text : Theme.Dim) : (Selected || hover ? Color.White : Theme.Dim);
        var flags = (Plain ? TextFormatFlags.Left : TextFormatFlags.HorizontalCenter) | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix;
        string txt = Text + (Chevrons ? "" : "");
        var tr = r; if (Chevrons) tr.Width -= 30;
        TextRenderer.DrawText(g, txt, Font, tr, fg, flags);
        int cy = r.Height / 2;
        if (Chevrons) using (var p = new Pen(Theme.Accent, 4)) {      // two chevrons, centred on the button
            for (int i = 0; i < 2; ++i) { int x = r.Right - 42 + i * 11; g.DrawLines(p, new[] { new Point(x, cy - 8), new Point(x + 8, cy), new Point(x, cy + 8) }); }
        }
        if (Arrow == 1) using (var b = new SolidBrush(Theme.Accent)) g.FillPolygon(b, new[] { new Point(r.Right - 30, cy - 7), new Point(r.Right - 20, cy), new Point(r.Right - 30, cy + 7) });
        if (Arrow == 2) using (var b = new SolidBrush(Theme.Accent)) g.FillPolygon(b, new[] { new Point(r.Right - 32, cy - 5), new Point(r.Right - 18, cy - 5), new Point(r.Right - 25, cy + 6) });
    }
}

// Check box in the same style: a striped box, amber when ticked, amber outline under the mouse.
class GameCheck : CheckBox {
    bool hover;
    public GameCheck() { SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer, true); Cursor = Cursors.Hand; }
    protected override void OnMouseEnter(EventArgs e) { hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { hover = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnPaint(PaintEventArgs e) {
        var g = e.Graphics;
        Theme.PaintBg(g, this);
        g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;
        int s = 18, y = (Height - s) / 2;
        var box = new Rectangle(1, y, s, s);
        Theme.Stripes(g, box, Color.FromArgb(52, 52, 48));
        using (var p = new Pen(hover ? Theme.Accent : Color.FromArgb(110, 112, 110), hover ? 2 : 1)) g.DrawRectangle(p, box);
        if (Checked) using (var p = new Pen(Theme.Accent, 3)) g.DrawLines(p, new[] { new Point(box.X + 4, y + 9), new Point(box.X + 8, y + 13), new Point(box.X + 15, y + 4) });
        TextRenderer.DrawText(g, Text, Font, new Rectangle(s + 10, 0, Width - s - 10, Height), hover || Checked ? Color.White : Theme.Dim, TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.NoPrefix);
        int tw = TextRenderer.MeasureText(g, Text, Font, Size, TextFormatFlags.NoPrefix).Width;
        using (var qf = new Font("Arial", 8f, FontStyle.Bold))       // hint marker: hover the line for details
            TextRenderer.DrawText(g, "?", qf, new Point(s + 10 + tw - 4, 2), Theme.Accent, TextFormatFlags.NoPrefix);
    }
}

// Panels that show the shared garage background (optionally dimmed), so everything on them looks
// translucent like the game's own menus.
class BgFlow : FlowLayoutPanel { public bool Dim; protected override void OnPaintBackground(PaintEventArgs e) { Theme.PaintBg(e.Graphics, this); } }
class BgPanel : Panel { protected override void OnPaintBackground(PaintEventArgs e) { Theme.PaintBg(e.Graphics, this); } }

static class Theme {
    public static Image Bg; public static Control BgRoot;
    public static void PaintBg(Graphics g, Control c) {
        using (var b = new SolidBrush(Back)) g.FillRectangle(b, c.ClientRectangle);
        if (Bg == null || BgRoot == null || !c.IsHandleCreated || !BgRoot.IsHandleCreated) return;
        Point a = c.PointToScreen(Point.Empty), r = BgRoot.PointToScreen(Point.Empty);
        g.DrawImage(Bg, r.X - a.X, r.Y - a.Y, Bg.Width, Bg.Height);
        for (Control p = c; p != null && p != BgRoot; p = p.Parent) {
            var f = p as BgFlow;
            if (f != null && f.Dim) { using (var b = new SolidBrush(Color.FromArgb(150, 6, 7, 8))) g.FillRectangle(b, c.ClientRectangle); break; }
        }
    }
    // The game's dark hazard-stripe panels: wide diagonal bands on near-black.
    public static void Stripes(Graphics g, Rectangle r, Color band) {
        var old = g.Clip; g.SetClip(r);
        using (var b = new SolidBrush(Color.FromArgb(165, 8, 8, 8))) g.FillRectangle(b, r);     // see-through, like the game's panels
        using (var b = new SolidBrush(Color.FromArgb(150, band)))
            for (int x = r.Left - r.Height; x < r.Right + 12; x += 16)
                g.FillPolygon(b, new[] { new Point(x, r.Bottom), new Point(x + 8, r.Bottom), new Point(x + 8 + r.Height, r.Top), new Point(x + r.Height, r.Top) });
        g.Clip = old;
    }
    public static readonly Color Back = Color.FromArgb(14, 16, 18), Panel = Color.FromArgb(28, 32, 36), Field = Color.FromArgb(40, 45, 50);
    public static readonly Color Text = Color.FromArgb(225, 228, 230), Dim = Color.FromArgb(140, 148, 155), Accent = Color.FromArgb(242, 169, 0);
    public static readonly Font Body = new Font("Segoe UI", 10f), Small = new Font("Segoe UI", 8.5f),
                                Head = new Font("Arial Black", 10.5f), Big = new Font("Georgia", 22f, FontStyle.Bold),
                                Label = new Font("Arial", 10f, FontStyle.Bold), Button = new Font("Arial", 11f, FontStyle.Bold);
}

class LauncherForm : Form {
    readonly string gameDir;
    const int W = 480;   // content width
    TextBox name = new TextBox(), hostIp = new TextBox(), port = new TextBox();
    GameButton hostBtn = new GameButton(), joinBtn = new GameButton(), play = new GameButton(), settingsBtn = new GameButton();
    bool isHost = true;
    Panel hostIpRow, settings;
    Label myIps = new Label();
    CheckBox skipIntro = Check("Skip intros"), direct = Check("Quick career start"),
             noSafeHouse = Check("Quick leave the safe house"), pursuitRes = Check("Auto-close pursuit results in free roam"),
             introInput = Check("Menus respond during animations"), overlay = Check("Show status overlay (F8)");
    GameButton[] speedBtns = { new GameButton(), new GameButton(), new GameButton() };
    int menuSpeed = 2;
    Label status = new Label { AutoSize = false, ForeColor = Theme.Accent, Font = Theme.Small };

    static CheckBox Check(string t) {
        return new GameCheck { Text = t, AutoSize = false, Size = new Size(W, 28), Font = Theme.Label, Margin = new Padding(0, 1, 0, 1) };
    }
    static Label Caption(string t) {
        return new Label { Text = t.ToUpperInvariant(), AutoSize = true, ForeColor = Color.White, BackColor = Color.Transparent, Font = Theme.Head, Margin = new Padding(0, 12, 0, 4) };
    }
    static void StyleField(TextBox tb) { tb.BorderStyle = BorderStyle.FixedSingle; tb.BackColor = Theme.Field; tb.ForeColor = Theme.Text; tb.Font = Theme.Body; }
    static void StyleButton(GameButton b, string text, bool selected) {
        b.Text = text; b.Cursor = Cursors.Hand; b.Font = Theme.Button; b.Selected = selected; b.Invalidate();
    }
    static Panel Row(int h) { return new BgPanel { Size = new Size(W, h), Margin = new Padding(0) }; }

    public LauncherForm(string dir) {
        gameDir = dir;
        Text = "NFS MW Together";
        FormBorderStyle = FormBorderStyle.FixedSingle; MaximizeBox = false;
        BackColor = Theme.Back; ForeColor = Theme.Text; Font = Theme.Body;
        StartPosition = FormStartPosition.CenterScreen;
        AutoSize = true; AutoSizeMode = AutoSizeMode.GrowAndShrink;
        try { Icon = Icon.ExtractAssociatedIcon(Path.Combine(dir, "speed.exe")); } catch { }

        var root = new FlowLayoutPanel { FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, Padding = new Padding(0), Margin = new Padding(0) };
        Controls.Add(root);

        // banner + title
        var banner = new PictureBox { Size = new Size(W + 40, 228), SizeMode = PictureBoxSizeMode.Zoom, Margin = new Padding(0), BackColor = Color.Black };
        try { var s = typeof(LauncherForm).Assembly.GetManifestResourceStream("banner.png"); if (s != null) banner.Image = Image.FromStream(s); } catch { }
        banner.SizeMode = PictureBoxSizeMode.StretchImage;
        root.Controls.Add(banner);
        var body = new BgFlow { FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, Padding = new Padding(20, 6, 20, 16), Margin = new Padding(0) };
        try { var s = typeof(LauncherForm).Assembly.GetManifestResourceStream("bg.jpg"); if (s != null) Theme.Bg = Image.FromStream(s); } catch { }
        Theme.BgRoot = body;
        root.Controls.Add(body);

        // name
        body.Controls.Add(Caption("Nickname"));
        StyleField(name); name.Size = new Size(W, 26); name.MaxLength = 15; name.Margin = new Padding(0);
        body.Controls.Add(name);

        // host / join
        var modeRow = Row(50);
        StyleButton(hostBtn, "Host LAN", false); hostBtn.SetBounds(0, 14, W / 2 - 4, 36);
        StyleButton(joinBtn, "Join LAN", false); joinBtn.SetBounds(W / 2 + 4, 14, W / 2 - 4, 36);
        hostBtn.Click += (s, e) => SetMode(true); joinBtn.Click += (s, e) => SetMode(false);
        modeRow.Controls.Add(hostBtn); modeRow.Controls.Add(joinBtn);
        body.Controls.Add(modeRow);

        hostIpRow = Row(58);
        var hl = Caption("Host's IP address"); hl.Location = new Point(0, 6); hostIpRow.Controls.Add(hl);
        StyleField(hostIp); hostIp.SetBounds(0, 30, W, 26); hostIpRow.Controls.Add(hostIp);
        body.Controls.Add(hostIpRow);


        // play
        StyleButton(play, "PLAY", true); play.Chevrons = true; play.Font = new Font("Arial Black", 17f);
        play.Size = new Size(W, 48); play.Margin = new Padding(0, 14, 0, 6);
        play.Click += (s, e) => Play();
        body.Controls.Add(play);
        status.Size = new Size(W, 18); status.Margin = new Padding(0); status.BackColor = Color.Transparent;
        body.Controls.Add(status);

        // settings (collapsed)
        StyleButton(settingsBtn, "SETTINGS", false); settingsBtn.Font = Theme.Head; settingsBtn.Arrow = 1;
        settingsBtn.Size = new Size(W, 34); settingsBtn.Margin = new Padding(0, 6, 0, 0);
        settingsBtn.Click += (s, e) => { settings.Visible = !settings.Visible; settingsBtn.Arrow = settings.Visible ? 2 : 1; settingsBtn.Invalidate(); };
        body.Controls.Add(settingsBtn);

        var sp = new BgFlow { Dim = true, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoSize = true, Padding = new Padding(12, 6, 12, 12), Margin = new Padding(0), Visible = false };
        settings = sp;
        var tip = new ToolTip { AutoPopDelay = 20000, InitialDelay = 250, ReshowDelay = 100, OwnerDraw = true };
        tip.Popup += (s, e) => { e.ToolTipSize = new Size(Math.Min(360, e.ToolTipSize.Width + 16), TextRenderer.MeasureText(tip.GetToolTip(e.AssociatedControl), Theme.Body, new Size(344, 0), TextFormatFlags.WordBreak).Height + 14); };
        tip.Draw += (s, e) => {
            using (var b = new SolidBrush(Color.FromArgb(20, 21, 22))) e.Graphics.FillRectangle(b, e.Bounds);
            using (var p = new Pen(Theme.Accent)) e.Graphics.DrawRectangle(p, 0, 0, e.Bounds.Width - 1, e.Bounds.Height - 1);
            TextRenderer.DrawText(e.Graphics, e.ToolTipText, Theme.Body, new Rectangle(8, 6, e.Bounds.Width - 16, e.Bounds.Height - 10), Theme.Text, TextFormatFlags.WordBreak);
        };
        string[] hints = {
            "Skips the EA logo, the warning screen and the intro movie when the game starts. Off by default. If the Widescreen Fix is installed, its own SkipIntro is switched off: it would also skip every story movie in the career.",
            "After launch the game presses START for you, loads your profile and goes straight into career free roam. No menus to click through.",
            "\"Resume Free Roam\" in the safe house starts at once, without the \"are you sure you want to leave?\" question.",
            "After a pursuit in free roam, the result screens (bounty achieved, pursuit summary) close by themselves. Only on this PC. The busted screen is left alone.",
            "Menu buttons react while a menu is still sliding in, so you don't have to wait for the animation.",
            "Shows the mod's status box in the top-left corner of the game: connection, ping and the other player. F8 switches it in game." };
        var checks = new CheckBox[] { skipIntro, direct, noSafeHouse, pursuitRes, introInput, overlay };
        for (int i = 0; i < checks.Length; ++i) { checks[i].Width = W - 24; sp.Controls.Add(checks[i]); tip.SetToolTip(checks[i], hints[i]); }
        var msRow = new BgPanel { Size = new Size(W - 24, 32), Margin = new Padding(0, 4, 0, 0) };
        msRow.Controls.Add(new Label { Text = "Menu animation speed", AutoSize = true, Location = new Point(0, 6), ForeColor = Theme.Text, BackColor = Color.Transparent, Font = Theme.Label });
        for (int i = 0; i < 3; ++i) {
            int v = i + 1;
            speedBtns[i].SetBounds(200 + i * 62, 0, 58, 28);
            speedBtns[i].Click += (s, e) => SetSpeed(v);
            msRow.Controls.Add(speedBtns[i]);
        }
        sp.Controls.Add(msRow);
        var colRow = new BgPanel { Size = new Size(W - 24, 32), Margin = new Padding(0, 2, 0, 0) };
        var colLbl = new Label { Text = "My colour on the map", AutoSize = true, Location = new Point(0, 6), ForeColor = Theme.Text, BackColor = Color.Transparent, Font = Theme.Label };
        colRow.Controls.Add(colLbl);
        tip.SetToolTip(colLbl, "The colour the other player sees your blip in, on the minimap and the world map. Game = the game's own colour.");
        swatch.SetBounds(200, 2, 58, 26); swatch.Cursor = Cursors.Hand;
        swatch.Click += (s, e) => PickColour();
        colRow.Controls.Add(swatch);
        var pick = new GameButton(); StyleButton(pick, "Pick", false); pick.SetBounds(262, 0, 58, 28); pick.Click += (s, e) => PickColour(); colRow.Controls.Add(pick);
        var gameCol = new GameButton(); StyleButton(gameCol, "Game", false); gameCol.SetBounds(324, 0, 58, 28); gameCol.Click += (s, e) => SetColour(""); colRow.Controls.Add(gameCol);
        sp.Controls.Add(colRow);
        var portRow = new BgPanel { Size = new Size(W - 24, 32), Margin = new Padding(0, 2, 0, 0) };
        portRow.Controls.Add(new Label { Text = "UDP port", AutoSize = true, Location = new Point(0, 6), ForeColor = Theme.Text, BackColor = Color.Transparent, Font = Theme.Label });
        StyleField(port); port.SetBounds(200, 2, 120, 26); portRow.Controls.Add(port);
        sp.Controls.Add(portRow);
        var btnRow = new BgPanel { Size = new Size(W - 24, 36), Margin = new Padding(0, 8, 0, 0) };
        var log = new GameButton(); StyleButton(log, "Open log", false); log.SetBounds(0, 0, (W - 24) / 2 - 4, 32);
        log.Click += (s, e) => OpenFile(Path.Combine(gameDir, "scripts", "MWTogether.log"));
        var ini = new GameButton(); StyleButton(ini, "All settings", false); ini.SetBounds((W - 24) / 2 + 4, 0, (W - 24) / 2 - 4, 32);
        ini.Click += (s, e) => OpenFile(Ini.Path);
        btnRow.Controls.Add(log); btnRow.Controls.Add(ini);
        sp.Controls.Add(btnRow);
        var fwRow = new BgPanel { Size = new Size(W - 24, 36), Margin = new Padding(0, 4, 0, 0) };
        var fw = new GameButton(); StyleButton(fw, "Allow through firewall", false); fw.SetBounds(0, 0, W - 24, 32);
        fw.Click += (s, e) => AllowFirewall();
        tip.SetToolTip(fw, "Lets the other player's game reach yours: adds a Windows Firewall rule for speed.exe and the UDP port. Windows asks for admin rights once. Needed when one of you can't see the other.");
        fwRow.Controls.Add(fw);
        sp.Controls.Add(fwRow);
        body.Controls.Add(sp);

        AcceptButton = play;
        LoadSettings();
    }

    void SetSpeed(int v) {
        menuSpeed = v;
        for (int i = 0; i < 3; ++i) StyleButton(speedBtns[i], (i + 1) + "x", i + 1 == v);
    }


    Panel swatch = new Panel { BorderStyle = BorderStyle.FixedSingle };
    string mapColour = "";       // RRGGBB, "" = the game's colour
    void SetColour(string hex) {
        mapColour = hex;
        int v;
        swatch.BackColor = hex.Length == 6 && int.TryParse(hex, System.Globalization.NumberStyles.HexNumber, null, out v)
            ? Color.FromArgb(255, (v >> 16) & 255, (v >> 8) & 255, v & 255) : Color.FromArgb(60, 60, 60);
    }
    void PickColour() {
        using (var d = new ColorDialog { FullOpen = true, Color = mapColour.Length == 6 ? swatch.BackColor : Color.Orange }) {
            if (d.ShowDialog(this) == DialogResult.OK) SetColour(string.Format("{0:X2}{1:X2}{2:X2}", d.Color.R, d.Color.G, d.Color.B));
        }
    }

    public void ForceMode(bool host) { SetMode(host); }
    public void ShowSettings() { settings.Visible = true; settingsBtn.Arrow = 2; }

    void SetMode(bool host) {
        isHost = host;
        StyleButton(hostBtn, "Host LAN", host); StyleButton(joinBtn, "Join LAN", !host);
        hostIpRow.Visible = !host;      // only joining needs the host's address
    }

    static string MyIps() {
        try {
            var list = NetworkInterface.GetAllNetworkInterfaces()
                .Where(n => n.OperationalStatus == OperationalStatus.Up && n.NetworkInterfaceType != NetworkInterfaceType.Loopback)
                .SelectMany(n => n.GetIPProperties().UnicastAddresses)
                .Where(a => a.Address.AddressFamily == AddressFamily.InterNetwork && !a.Address.ToString().StartsWith("169.254."))   // not the self-assigned ones nobody can reach
                .Select(a => a.Address.ToString()).Distinct().ToArray();
            return list.Length > 0 ? string.Join(", ", list) : "?";
        } catch { return "?"; }
    }

    void AllowFirewall() {
        int p; if (!int.TryParse(port.Text.Trim(), out p) || p < 1 || p > 65535) p = 47555;
        string exe = Path.Combine(gameDir, "speed.exe").Replace("'", "''");   // PowerShell single-quoted string
        string cmd = "Remove-NetFirewallRule -DisplayName 'NFS MW Together*' -ErrorAction SilentlyContinue; " +
                     "New-NetFirewallRule -DisplayName 'NFS MW Together (game)' -Direction Inbound -Program '" + exe + "' -Action Allow -Profile Any | Out-Null; " +
                     "New-NetFirewallRule -DisplayName 'NFS MW Together (UDP " + p + ")' -Direction Inbound -Protocol UDP -LocalPort " + p + " -Action Allow -Profile Any | Out-Null";
        try {
            var psi = new ProcessStartInfo("powershell.exe", "-NoProfile -WindowStyle Hidden -Command \"" + cmd + "\"") { Verb = "runas", UseShellExecute = true, WindowStyle = ProcessWindowStyle.Hidden };
            var pr = Process.Start(psi); pr.WaitForExit(20000);
            status.Text = pr.ExitCode == 0 ? "Firewall: the game and UDP " + p + " are allowed now." : "Firewall rule failed (code " + pr.ExitCode + ").";
        } catch (Exception ex) { status.Text = "Firewall not changed: " + ex.Message; }
    }

    void OpenFile(string p) { try { if (File.Exists(p)) Process.Start("notepad.exe", "\"" + p + "\""); else status.Text = "Not found yet: " + p; } catch (Exception ex) { status.Text = ex.Message; } }

    void LoadSettings() {
        name.Text = Ini.Get("Player", "Name", Environment.UserName);
        SetColour(Ini.Get("Player", "Color", "").Trim().TrimStart('#').ToUpperInvariant());
        hostIp.Text = Ini.Get("Network", "HostIP", "");
        port.Text = Ini.Get("Network", "Port", "47555");
        skipIntro.Checked = Ini.GetB("Start", "SkipIntro", false);
        direct.Checked = Ini.GetB("Start", "DirectFreeRoam", true);
        noSafeHouse.Checked = Ini.GetB("Start", "SkipLeaveSafeHouseDialog", true);
        pursuitRes.Checked = Ini.GetB("UI", "SkipPursuitResults", false);
        introInput.Checked = Ini.GetB("UI", "IntroInput", true);
        overlay.Checked = Ini.GetB("Debug", "Overlay", false);
        int ms; if (!int.TryParse(Ini.Get("UI", "MenuSpeed", "2"), out ms)) ms = 2;
        SetSpeed(Math.Max(1, Math.Min(3, ms)));
        SetMode(!Ini.Get("Network", "Mode", "host").Trim().Equals("client", StringComparison.OrdinalIgnoreCase));
    }

    bool Save() {
        int p;
        if (!int.TryParse(port.Text.Trim(), out p) || p < 1 || p > 65535) { status.Text = "The port must be a number from 1 to 65535."; return false; }
        if (!isHost && hostIp.Text.Trim().Length == 0) { status.Text = "Enter the host's IP address to join."; return false; }
        if (name.Text.Trim().Length == 0) { status.Text = "Enter your name."; return false; }
        Ini.Set("Player", "Name", name.Text.Trim());
        Ini.Set("Player", "Color", mapColour);
        Ini.Set("Network", "Mode", isHost ? "host" : "client");
        if (!isHost) Ini.Set("Network", "HostIP", hostIp.Text.Trim());
        Ini.Set("Network", "Port", p.ToString());
        Ini.SetB("Start", "SkipIntro", skipIntro.Checked);
        WidescreenFixKeepMovies();
        Ini.SetB("Start", "DirectFreeRoam", direct.Checked);
        Ini.SetB("Start", "SkipLeaveSafeHouseDialog", noSafeHouse.Checked);
        Ini.SetB("UI", "SkipPursuitResults", pursuitRes.Checked);
        Ini.SetB("UI", "IntroInput", introInput.Checked);
        Ini.Set("UI", "MenuSpeed", menuSpeed.ToString());
        Ini.SetB("Debug", "Overlay", overlay.Checked);
        return true;
    }

    // The Widescreen Fix's own SkipIntro = 1 skips every movie the game plays, the career's story movies
    // too. MW Together's "Skip intros" does the intro part, so the fix's option is kept at 0 (the line's
    // comment is kept). Nothing happens when the fix isn't installed.
    void WidescreenFixKeepMovies() {
        string f = Path.Combine(Path.GetDirectoryName(Ini.Path), "NFSMostWanted.WidescreenFix.ini");
        try {
            if (!File.Exists(f)) return;
            var lines = File.ReadAllLines(f);
            bool changed = false;
            for (int i = 0; i < lines.Length; ++i) {
                var m = System.Text.RegularExpressions.Regex.Match(lines[i], @"^(\s*SkipIntro\s*=\s*)([0-9]+)(.*)$", System.Text.RegularExpressions.RegexOptions.IgnoreCase);
                if (m.Success && m.Groups[2].Value != "0") { lines[i] = m.Groups[1].Value + "0" + m.Groups[3].Value; changed = true; }
            }
            if (changed) File.WriteAllLines(f, lines);
        } catch { }
    }

    void Play() {
        if (!Save()) return;
        string exe = Path.Combine(gameDir, "speed.exe");
        if (Process.GetProcessesByName("speed").Length > 0) { status.Text = "The game is already running."; return; }
        try {
            Process.Start(new ProcessStartInfo(exe) { WorkingDirectory = gameDir, UseShellExecute = true });
            Close();
        } catch (Exception ex) { status.Text = "Couldn't start the game: " + ex.Message; }
    }
}

static class Program {
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [STAThread]
    static void Main() {
        string dir = AppDomain.CurrentDomain.BaseDirectory;
        Application.EnableVisualStyles();
        if (!File.Exists(Path.Combine(dir, "speed.exe"))) {
            MessageBox.Show("Put this launcher next to speed.exe (the NFS Most Wanted folder).", "NFS MW Together");
            return;
        }
        string scripts = Path.Combine(dir, "scripts");
        Ini.Path = Path.Combine(scripts, "MWTogether.ini");
        if (!File.Exists(Ini.Path)) {
            string old = Path.Combine(scripts, "CoopMod.ini");
            string def = Path.Combine(scripts, "MWTogether.default.ini");   // shipped instead of MWTogether.ini: updates keep your settings
            if (File.Exists(old)) File.Copy(old, Ini.Path);
            else if (File.Exists(def)) File.Copy(def, Ini.Path);
            else if (!File.Exists(Path.Combine(scripts, "MWTogether.asi"))) {
                MessageBox.Show("NFS MW Together isn't installed: scripts\\MWTogether.asi is missing.", "NFS MW Together");
                return;
            }
        }
        var args = Environment.GetCommandLineArgs();
        if (args.Length >= 3 && args[1] == "--screenshot") {   // render to a PNG and exit (docs / checks)
            var f = new LauncherForm(dir);
            string opts = string.Join(" ", args, 3, args.Length - 3);   // e.g. "settings join"
            if (opts.Contains("join")) f.ForceMode(false); else if (opts.Contains("host")) f.ForceMode(true);
            if (opts.Contains("settings")) f.ShowSettings();
            f.StartPosition = FormStartPosition.Manual; f.Location = new Point(-4000, -4000); f.ShowInTaskbar = false;
            f.Show(); for (int i = 0; i < 20; ++i) { Application.DoEvents(); System.Threading.Thread.Sleep(15); }
            var bmp = new Bitmap(f.Width, f.Height);
            f.DrawToBitmap(bmp, new Rectangle(0, 0, f.Width, f.Height));
            bmp.Save(args[2], System.Drawing.Imaging.ImageFormat.Png);
            return;
        }
        Application.Run(new LauncherForm(dir));
    }
}
