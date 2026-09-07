using System.Diagnostics;
using System.Text.Json;
using System.Text;

namespace CpeRelay.Windows;

internal sealed class MainForm : Form
{
    private readonly RelayBackend backend;
    private readonly AppSettings settings;
    private readonly FloatingDepositForm floating = new();
    private readonly System.Windows.Forms.Timer pollTimer = new() { Interval = 500 };
    private readonly System.Windows.Forms.Timer configTimer = new() { Interval = 250 };
    private readonly System.Windows.Forms.Timer overlayTimer = new() { Interval = 250 };
    private readonly TextBox host = new() { Width = 370 };
    private readonly NumericUpDown port = new() { Minimum = 1, Maximum = 65535, Width = 125 };
    private readonly ComboBox version = new RelayComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 220 };
    private readonly TextBox directory = new() { ReadOnly = true, Width = 555 };
    private readonly TextBox slot = new() { Width = 260, Text = "default", MaxLength = 32 };
    private readonly TextBox profile = new() { Width = 370, MaxLength = 32 };
    private readonly ListBox profiles = new() { Width = 738, Height = 145, IntegralHeight = false };
    private readonly Label profilePath = Label("");
    private readonly Label profileFeedback = Label("Токены появятся после успешного входа Microsoft.");
    private readonly Label profileBadge = Label("");
    private readonly ListBox nbtFiles = new() { Width = 738, Height = 145, IntegralHeight = false };
    private readonly TextBox log = new() { Multiline = true, ReadOnly = true, Dock = DockStyle.Fill, ScrollBars = ScrollBars.Vertical,
        BackColor = Color.FromArgb(24, 35, 50), ForeColor = Color.FromArgb(218, 230, 240), Font = new Font("Consolas", 10) };
    private readonly CheckBox deposit = Check("Автоматически перекладывать шалкеры в открытый сундук");
    private readonly CheckBox hotbar = Check("Включать шалкеры из хотбара (слоты 1–9)");
    private readonly CheckBox armor = Check("Автоброня");
    private readonly CheckBox totem = Check("Автототем");
    private readonly CheckBox floatingEnabled = Check("Панель поверх окон, в том числе Minecraft и сундуков");
    private readonly CheckBox detailed = Check("Подробный журнал");
    private readonly TrackBar speed = new() { Minimum = 0, Maximum = 297, TickFrequency = 30, Width = 600, Height = 48 };
    private readonly Label speedLabel = Label("Пауза между переносами: 1000 мс");
    private readonly Label depositStatus = Label("Разгрузка выключена");
    private readonly Label nbtStatus = Label("NBT-крафт выключен");
    private readonly Label nbtFeedback = Label("");
    private readonly Label fileCount = Label("Нет файлов. Импортируйте .qznbt с ПК или выберите другую папку.");
    private readonly Label status = new() { Text = "Реле остановлено", AutoSize = true, ForeColor = Theme.Muted, Padding = new Padding(12) };
    private readonly Label memory = new() { AutoSize = true, ForeColor = Color.LightGray, Padding = new Padding(12) };
    private readonly Label auth = Label("При первом входе Minecraft здесь появится код Microsoft.");
    private readonly Button start = Button("▶  Запустить реле", true);
    private readonly Button stop = Button("■  Остановить");
    private readonly Button folderButton = Button("Выбрать папку…");
    private bool initialized, busy, polling, running, closing, closeAllowed, starting, stopping;
    private int lifecycle;
    private readonly bool preview;
    private readonly bool lifecycleTest;
    private readonly Panel pages = new() { Dock = DockStyle.Fill, BackColor = Theme.Background };
    private readonly List<RelayButton> navigation = [];
    private readonly List<Panel> screens = [];

    internal MainForm(bool preview = false, RelayBackend? testBackend = null)
    {
        this.preview = preview;
        backend = testBackend ?? new RelayBackend();
        lifecycleTest = testBackend != null;
        settings = preview ? new AppSettings() : AppSettings.Load();
        Text = "CPE Relay — Windows 1.0.1";
        AutoScaleMode = AutoScaleMode.Dpi;
        Font = new Font("Segoe UI", 10);
        Size = new Size(1080, 790); MinimumSize = new Size(860, 700);
        StartPosition = FormStartPosition.CenterScreen;
        BackColor = Theme.Background; ForeColor = Theme.Text;
        Size = new Size(1210, 930); MinimumSize = new Size(1100, 810);
        var sidebar = new Panel { Dock = DockStyle.Left, Width = 226, BackColor = Theme.Sidebar, Padding = new Padding(16, 0, 16, 20) };
        var brand = new BrandPanel();
        var nav = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, Padding = new Padding(0, 12, 0, 0) };
        string[] names = ["01    Подключение", "02    Профили", "03    Библиотека NBT", "04    Модули", "05    Журнал"];
        for (int i = 0; i < names.Length; i++)
        {
            int page = i;
            var button = new RelayButton { Text = names[i], Navigation = true, Width = 194, Height = 51, Margin = new Padding(0, 0, 0, 7) };
            button.Click += (_, _) => SelectPage(page); navigation.Add(button); nav.Controls.Add(button);
        }
        var signature = new Label { Text = "LOCAL RELAY\nWindows x64  /  1.0.1", Dock = DockStyle.Bottom, Height = 52, ForeColor = Theme.Muted, Padding = new Padding(13, 8, 0, 0), Font = new Font("Segoe UI", 9) };
        sidebar.Controls.Add(nav); sidebar.Controls.Add(signature); sidebar.Controls.Add(brand);
        var footer = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 77, Padding = new Padding(26, 12, 0, 0), BackColor = Theme.Sidebar, WrapContents = false };
        footer.Controls.AddRange([start, stop, status, memory]);
        screens.AddRange([BuildConnection(), BuildProfiles(), BuildNbt(), BuildModules(), BuildLogs()]);
        foreach (var screen in screens) { screen.Dock = DockStyle.Fill; pages.Controls.Add(screen); }
        var workspace = new Panel { Dock = DockStyle.Fill }; workspace.Controls.Add(pages); workspace.Controls.Add(footer);
        Controls.Add(workspace); Controls.Add(sidebar);
        Theme.Inputs(this); SelectPage(0);
        HandleCreated += (_, _) => Theme.DarkTitle(this);
        host.Text = settings.Host; port.Value = settings.Port; directory.Text = settings.NbtDirectory;
        profile.Text = settings.AuthProfile; RefreshProfiles(); UpdateProfilePath();
        profile.TextChanged += (_, _) => { UpdateProfilePath(); ScheduleConfig(); };
        version.Items.Add(settings.Version); version.SelectedIndex = 0;
        deposit.Checked = settings.Deposit; hotbar.Checked = settings.Hotbar; armor.Checked = settings.Armor;
        totem.Checked = settings.Totem; detailed.Checked = settings.Logging; floatingEnabled.Checked = settings.FloatingButton;
        speed.Value = (3000 - AppSettings.ClampInterval(settings.IntervalMs)) / 10;
        UpdateSpeed();
        speed.ValueChanged += (_, _) => { UpdateSpeed(); ScheduleConfig(); };
        foreach (var check in new[] { deposit, hotbar, armor, totem, detailed, floatingEnabled }) check.CheckedChanged += (_, _) => ScheduleConfig();
        start.Click += async (_, _) => await StartRelay();
        stop.Click += async (_, _) => await StopRelay();
        floating.Toggle = () => deposit.Checked = !deposit.Checked;
        configTimer.Tick += async (_, _) => { configTimer.Stop(); await ApplySettings(); };
        pollTimer.Tick += async (_, _) => await Poll();
        overlayTimer.Tick += (_, _) => floating.UpdateVisibility(running && !closing && !stopping, floatingEnabled.Checked);
        if (!preview) overlayTimer.Start();
        Shown += async (_, _) => { if (!preview) await Initialize(); };
        FormClosing += OnClosing;
        FormClosed += (_, _) => { pollTimer.Dispose(); configTimer.Dispose(); overlayTimer.Dispose(); floating.Dispose(); backend.Dispose(); };
        RefreshFiles(); SetBusy(false);
    }

    internal void SelectPage(int index)
    {
        for (int i = 0; i < screens.Count; i++)
        {
            screens[i].Visible = i == index; navigation[i].Selected = i == index; navigation[i].Invalidate();
        }
        screens[index].BringToFront();
    }
    private static CheckBox Check(string text) => new ToggleCheckBox { Text = text, Margin = new Padding(0, 5, 0, 5) };
    private static Label Label(string text) => new() { Text = text, AutoSize = true, ForeColor = Theme.Muted, MaximumSize = new Size(748, 0), Margin = new Padding(0, 5, 0, 7) };
    private static Button Button(string text, bool primary = false) => new RelayButton { Text = text, Primary = primary, AutoSize = true,
        Height = 42, MinimumSize = new Size(130, 42), Padding = new Padding(14, 6, 14, 6), Margin = new Padding(0, 3, 10, 6), UseVisualStyleBackColor = false };
    private static FlowLayoutPanel Stack() => new() { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown,
        WrapContents = false, AutoScroll = true, BackColor = Theme.Background, Padding = new Padding(30, 24, 20, 16) };
    private static FlowLayoutPanel Row(params Control[] controls)
    {
        var row = new FlowLayoutPanel { AutoSize = true, WrapContents = false, Margin = new Padding(0, 2, 0, 2) };
        row.Controls.AddRange(controls); return row;
    }
    private static RelayCard Card(Control parent, params Control[] children)
    {
        var card = new RelayCard(); card.Controls.AddRange(children);
        // All cards share a visual edge; contents are sized independently.
        card.MinimumSize = new Size(804, 0);
        parent.Controls.Add(card); return card;
    }
    private static void Heading(Control root, string title, string subtitle)
    {
        root.Controls.Add(new Label { Text = title, AutoSize = true, ForeColor = Theme.Text, Font = new Font("Segoe UI Semibold", 25), Margin = new Padding(0, 0, 0, 6) });
        var sub = Label(subtitle); sub.Margin = new Padding(0, 0, 0, 23); root.Controls.Add(sub);
    }
    private static Label Section(string text) => new() { Text = text, AutoSize = true, ForeColor = Theme.Accent,
        Font = new Font("Segoe UI Semibold", 9), Margin = new Padding(0, 1, 0, 12) };
    private Panel BuildConnection()
    {
        var tab = new Panel(); var body = Stack(); tab.Controls.Add(body);
        Heading(body, "Всё готово к игре.", "Ваш сервер, привычные NBT и инструменты — в одном месте.");
        Card(body, Section("01  /  СЕРВЕР"), Label("Адрес сервера и порт"), Row(host, port),
            Label("Версия Minecraft Bedrock"), version,
            Label("Выберите версию установленной игры. Реле не преобразует протокол между версиями."));
        var launch = Button("Открыть Minecraft", true); launch.Click += (_, _) => OpenShell("minecraft://");
        var copy = Button("Копировать адрес"); copy.Click += (_, _) => Clipboard.SetText("127.0.0.1");
        var help = Button("Инструкция"); help.Click += (_, _) => OpenShell(Path.Combine(AppContext.BaseDirectory, "README.txt"));
        Card(body, Section("02  /  ПОДКЛЮЧЕНИЕ ИГРЫ"),
            new Label { Text = "127.0.0.1  :  19132", AutoSize = true, ForeColor = Theme.Text, Font = new Font("Consolas", 21), Margin = new Padding(0, 0, 0, 9) },
            Label("Запустите реле кнопкой внизу. В Minecraft добавьте сервер с этим адресом и портом."),
            Row(launch, copy, help), Label("Если Windows блокирует локальное подключение, откройте «Инструкция»."));
        var login = Button("Открыть страницу входа"); login.Click += (_, _) => OpenShell("https://www.microsoft.com/link");
        Card(body, Section("03  /  MICROSOFT"), profileBadge, auth, login);
        return tab;
    }
    private Panel BuildProfiles()
    {
        var tab = new Panel(); var body = Stack(); tab.Controls.Add(body);
        Heading(body, "Ваш профиль — под рукой.", "Сохраняйте авторизацию по нику и загружайте свою папку токенов перед входом.");
        profiles.SelectedIndexChanged += (_, _) => { if (!running && profiles.SelectedItem is string name) profile.Text = name; };
        var open = Button("Открыть папку профиля");
        open.Click += (_, _) => { try { OpenShell(AuthProfiles.Ensure(profile.Text.Trim())); RefreshProfiles(); } catch (Exception error) { ShowError(error); } };
        Card(body, Section("ВЫБРАННЫЙ ПРОФИЛЬ"), Label("Ник или название профиля · до 32 английских букв, цифр, пробелов, _ или -"),
            profile, profilePath, open, Label("Это имя локальной папки, а не изменение ника Minecraft. Токены сохраняются автоматически после входа."));
        var refresh = Button("Обновить список"); refresh.Click += (_, _) => RefreshProfiles();
        Card(body, Section("СОХРАНЁННЫЕ ПРОФИЛИ"), profiles, refresh);
        var import = Button("Загрузить папку токенов", true); import.Click += async (_, _) => await TransferProfile(false);
        var export = Button("Сохранить папку…"); export.Click += async (_, _) => await TransferProfile(true);
        Card(body, Section("ПЕРЕНОС АВТОРИЗАЦИИ"), Row(import, export), profileFeedback,
            Label("Импортируйте кэш реле с файлами *-cache.json. Нужен полный профиль Live, не одиночный токен.\nСначала остановите реле. Можно загрузить в существующий профиль: старый кэш останется в auth-backup."));
        body.Controls.Add(Label("Папка содержит доступ к аккаунту. Храните её у себя и никому не передавайте.\nИстёкшие или отозванные токены могут потребовать повторного входа Microsoft."));
        return tab;
    }
    private void UpdateProfilePath()
    {
        string name = profile.Text.Trim();
        profilePath.Text = AuthProfiles.ValidName(name) ? AuthProfiles.Folder(name) : "Укажите допустимое имя профиля.";
        bool cached = false;
        try { cached = AuthProfiles.HasCache(name); } catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        profileBadge.Text = "Профиль: " + name + (cached ? "  ·  кэш найден (вход ещё не проверен)" : "  ·  кэш не загружен — выберите «Профили»");
    }
    private void RefreshProfiles()
    {
        profiles.Items.Clear();
        try
        {
            if (!Directory.Exists(AuthProfiles.Root)) return;
            foreach (var item in Directory.EnumerateDirectories(AuthProfiles.Root).Take(500).Select(Path.GetFileName).Where(AuthProfiles.ValidName).OrderBy(x => x))
                profiles.Items.Add(item!);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { ShowError(error); }
    }
    private async Task TransferProfile(bool export)
    {
        if (busy) return;
        if (running) { ShowError(new InvalidOperationException("Остановите реле перед переносом токенов.")); return; }
        string name = profile.Text.Trim();
        if (!AuthProfiles.ValidName(name)) { ShowError(new InvalidOperationException("Сначала укажите допустимый ник/имя профиля.")); return; }
        using var dialog = new FolderBrowserDialog { Description = export ? "Куда сохранить папку с токенами? Не передавайте её другим." : "Папка профиля или auth с файлами *-cache.json", UseDescriptionForTitle = true };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        if (!export && Directory.Exists(AuthProfiles.Folder(name)) && MessageBox.Show(this,
            $"Загрузить токены в профиль «{name}»? Прежняя папка auth будет сохранена рядом как auth-backup. После импорта этот профиль будет выбран для входа.",
            "Загрузка профиля", MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
        SetBusy(true);
        try
        {
            string selected = dialog.SelectedPath;
            int count = await Task.Run(() => export ? AuthProfiles.Import(AuthProfiles.Folder(name), name, selected) : AuthProfiles.Import(selected, name, replaceExisting: true));
            if (closing) return;
            profile.Text = name;
            profileFeedback.Text = export ? $"Сохранена папка «{name}» ({count} файлов). Храните её в секрете." : $"Загружен профиль «{name}» ({count} файлов). Можно запускать реле.";
            RefreshProfiles(); profiles.SelectedItem = name; UpdateProfilePath(); ReadSettings(); settings.Save();
        }
        catch (Exception error) { ShowError(error); }
        finally { SetBusy(false); }
    }
    private Panel BuildNbt()
    {
        var tab = new Panel(); var body = Stack(); tab.Controls.Add(body);
        Heading(body, "Библиотека NBT", "Ваши шалкеры с ПК. Выберите файл — и используйте его при ручном крафте.");
        folderButton.Click += (_, _) => ChooseFolder();
        nbtFiles.SelectedIndexChanged += (_, _) => { if (nbtFiles.SelectedItem is string file) slot.Text = SlotFromFile(file); };
        nbtFiles.DrawMode = DrawMode.OwnerDrawFixed; nbtFiles.ItemHeight = 31;
        nbtFiles.DrawItem += (_, e) =>
        {
            if (e.Index < 0) return;
            bool selected = (e.State & DrawItemState.Selected) != 0;
            using var fill = new SolidBrush(selected ? Color.FromArgb(53, 44, 78) : Theme.Input); e.Graphics.FillRectangle(fill, e.Bounds);
            TextRenderer.DrawText(e.Graphics, "  ◇   " + nbtFiles.Items[e.Index], nbtFiles.Font, e.Bounds, selected ? Theme.Accent : Theme.Text,
                TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis);
            e.DrawFocusRectangle();
        };
        var import = Button("Импортировать…", true); import.Click += (_, _) => ImportFile();
        var refresh = Button("Обновить"); refresh.Click += (_, _) => RefreshFiles();
        var open = Button("Открыть папку"); open.Click += (_, _) => { Directory.CreateDirectory(directory.Text); OpenShell(directory.Text); };
        Card(body, Section("ФАЙЛЫ  /  .QZNBT  +  .CPENBT.JSON"), Row(directory, folderButton), nbtFiles, fileCount, Row(import, refresh, open));
        var save = Button("Сохранить из руки"); save.Click += async (_, _) => await NbtAction("nbt.copy");
        var arm = Button("Включить NBT-крафт", true); arm.Click += async (_, _) => await NbtAction("nbt.arm");
        var off = Button("Выключить"); off.Click += async (_, _) => await NbtAction("nbt.off");
        Card(body, Section("КРАФТ  /  НЕПРЕРЫВНЫЙ РЕЖИМ"), Label("Имя слота · A–Z, цифры, _ или - · до 32 символов"),
            slot, Row(arm, save, off), nbtStatus, nbtFeedback,
            Label("Создавайте шалкеры в верстаке вручную. Выбранный NBT применяется до выключения режима."));
        Card(body, Section("БЫСТРЫЕ КОМАНДЫ"), Label(".nbt copy    ·    .nbt save имя    ·    .nbt craft имя    ·    .nbt off"));
        return tab;
    }
    private Panel BuildModules()
    {
        var tab = new Panel(); var body = Stack(); tab.Controls.Add(body);
        Heading(body, "Меньше действий.", "Настройте разгрузку и снаряжение под свой темп игры.");
        Card(body, Section("АВТОРАЗГРУЗКА ШАЛКЕРОВ"), deposit, hotbar, floatingEnabled,
            Label("Откройте сундук: используются только свободные ячейки."), depositStatus);
        speed.Width = 734; speed.TickStyle = TickStyle.None;
        Card(body, Section("СКОРОСТЬ"), speedLabel, speed, Label("Медленнее  ←                                                               →  Быстрее"),
            Label("30–3000 мс между переносами. Для тяжёлых нестедов начните с 1000 мс.\nФактическая скорость зависит от подтверждений сервера."));
        Card(body, Section("СНАРЯЖЕНИЕ"), armor, totem);
        body.Controls.Add(Label("Плавающая кнопка остаётся в сундуках. Перемещайте её за верхнюю полоску.\nИспользуйте оконный или безрамочный Minecraft. Ручное перемещение приостанавливает\nразгрузку до повторного открытия сундука."));
        return tab;
    }
    private Panel BuildLogs()
    {
        var tab = new Panel(); var body = new Panel { Dock = DockStyle.Fill, Padding = new Padding(30, 24, 30, 24), BackColor = Theme.Background };
        tab.Controls.Add(body);
        var top = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 154, FlowDirection = FlowDirection.TopDown, WrapContents = false };
        Heading(top, "Всё под контролем.", "События подключения и модулей. История ограничена, чтобы не накапливать память.");
        detailed.Width = 230;
        var open = Button("Папка журнала"); open.Click += (_, _) => { Directory.CreateDirectory(AppSettings.DirectoryPath); OpenShell(AppSettings.DirectoryPath); };
        var clear = Button("Очистить экран"); clear.Click += (_, _) => log.Clear();
        top.Controls.Add(Row(detailed, open, clear));
        log.BackColor = Theme.Background; body.Controls.Add(log); body.Controls.Add(top);
        return tab;
    }
    private async Task Initialize()
    {
        int operation = lifecycle;
        SetBusy(true);
        try
        {
            var versions = await backend.Call(new { action = "versions" });
            if (closing || operation != lifecycle) return;
            version.Items.Clear();
            foreach (var value in versions.EnumerateArray()) version.Items.Add(value.GetString()!);
            version.SelectedItem = settings.Version;
            if (version.SelectedIndex < 0) version.SelectedIndex = version.Items.Count - 1;
            initialized = true; await ApplySettings();
            if (!closing && operation == lifecycle) pollTimer.Start();
        }
        catch (Exception error) { ShowError(error); }
        finally { if (operation == lifecycle) SetBusy(false); }
    }
    private void UpdateSpeed() => speedLabel.Text = $"Пауза между переносами: {3000 - speed.Value * 10} мс";
    private void ReadSettings()
    {
        settings.Host = host.Text.Trim(); settings.Port = (int)port.Value; settings.Version = version.Text;
        settings.AuthProfile = profile.Text.Trim();
        settings.NbtDirectory = directory.Text; settings.Deposit = deposit.Checked; settings.Hotbar = hotbar.Checked;
        settings.Armor = armor.Checked; settings.Totem = totem.Checked; settings.Logging = detailed.Checked;
        settings.FloatingButton = floatingEnabled.Checked; settings.IntervalMs = 3000 - speed.Value * 10;
    }
    private void ScheduleConfig() { if (!initialized || closing || stopping) return; configTimer.Stop(); configTimer.Start(); }
    private async Task<bool> ApplySettings()
    {
        if (closing || stopping) return false;
        try
        {
            ReadSettings(); if (!preview) settings.Save();
            await backend.Call(new { action = "configure", deposit = settings.Deposit, hotbar = settings.Hotbar,
                armor = settings.Armor, totem = settings.Totem, logging = settings.Logging, intervalMs = settings.IntervalMs });
            return !closing && !stopping;
        }
        catch (Exception error) { ShowError(error); return false; }
    }
    private void SetBusy(bool value)
    {
        if (closing || IsDisposed) return;
        busy = value; start.Enabled = !value && !running && !stopping;
        stop.Enabled = !stopping && (starting || running);
        host.Enabled = port.Enabled = version.Enabled = folderButton.Enabled = !value && !running && !stopping;
        profile.Enabled = profiles.Enabled = !value && !running && !stopping;
    }
    private async Task StartRelay()
    {
        if (busy || !initialized || closing || stopping) return;
        int operation = ++lifecycle;
        starting = true;
        SetBusy(true); status.Text = "Запуск…";
        try
        {
            if (!await ApplySettings() || closing || operation != lifecycle) return;
            if (!preview) AuthProfiles.Ensure(settings.AuthProfile);
            string data = Path.Combine(AppContext.BaseDirectory, "minecraft-data", version.Text);
            if (!Directory.Exists(data)) data = "";
            var result = await backend.Call(new { action = "start", host = settings.Host, port = settings.Port,
                version = settings.Version, directory = AppSettings.DirectoryPath, nbtDirectory = settings.NbtDirectory,
                authProfile = settings.AuthProfile,
                minecraftDataDirectory = data });
            if (closing || operation != lifecycle) return;
            UpdateState(result); AddLog("Реле запущено. В Minecraft подключитесь к 127.0.0.1:19132.");
        }
        catch (Exception error) { if (!closing && operation == lifecycle) { running = false; status.Text = "Ошибка запуска"; ShowError(error); } }
        finally { if (operation == lifecycle) { starting = false; SetBusy(false); } }
    }
    private async Task StopRelay()
    {
        if (closing || stopping) return;
        ++lifecycle; stopping = true;
        pollTimer.Stop(); configTimer.Stop(); floating.Hide();
        SetBusy(true); status.Text = "Остановка…";
        try
        {
            bool forced = await backend.StopAsync();
            if (closing) return;
            running = false; status.Text = "Реле остановлено";
            auth.Text = "При первом входе Minecraft здесь появится код Microsoft.";
            nbtStatus.Text = "NBT-крафт выключен";
            AddLog(forced ? "Ядро не ответило за 3 секунды и было завершено. Можно запустить реле снова." : "Реле остановлено.");
        }
        catch (Exception error) { ShowError(error); }
        finally { starting = false; stopping = false; SetBusy(false); if (!closing && initialized) pollTimer.Start(); }
    }
    private async Task Poll()
    {
        if (polling || closing || stopping || preview) return;
        int operation = lifecycle;
        polling = true;
        try
        {
            var result = await backend.Poll();
            if (result is null || closing || stopping || operation != lifecycle) return;
            UpdateState(result.Value.State);
            var messages = new List<string>();
            foreach (var item in result.Value.Events.EnumerateArray())
            {
                if (item.Text("type") == "msa_code")
                {
                    auth.Text = "Откройте microsoft.com/link и введите код: " + item.Text("userCode");
                    continue; // Device code must never enter persistent diagnostics.
                }
                messages.Add($"[{item.Text("level", "INFO")}] {item.Text("type")}: {item.Text("message")}");
            }
            AddLogs(messages);
            using var process = Process.GetCurrentProcess();
            memory.Text = $"Окно: {process.PrivateMemorySize64 / 1048576} МБ";
        }
        catch (Exception error) { if (!closing && operation == lifecycle) AddLog(error.Message); }
        finally { polling = false; }
    }
    private void UpdateState(JsonElement state)
    {
        running = state.Flag("running");
        if (!busy) status.Text = !running ? "Реле остановлено" : state.Flag("upstreamReady") ? "Подключено к серверу" :
            state.Flag("upstreamStarted") ? "Авторизация / подключение к серверу" : "Ожидание Minecraft";
        if (state.Flag("upstreamReady")) auth.Text = "Вход выполнен. Код больше не нужен.";
        nbtStatus.Text = state.Flag("nbtCraftArmed") ? "NBT-крафт активен: " + state.Text("nbtCraftSlot") : "NBT-крафт выключен";
        if (state.TryGetProperty("shulkerDeposit", out var value))
            depositStatus.Text = value.Flag("supported") ? value.Text("status") + "  ·  Отправлено: " + value.GetProperty("sent") + "  ·  Подтверждено: " + value.GetProperty("confirmed") : "Авторазгрузка недоступна для выбранного формата пакетов.";
        floating.UpdateIndicators(state, deposit.Checked);
        if (!busy) SetBusy(false);
    }
    private static string SlotFromFile(string file) => file.EndsWith(".cpenbt.json", StringComparison.OrdinalIgnoreCase)
        ? file[..^12] : Path.GetFileNameWithoutExtension(file);
    private void RefreshFiles()
    {
        nbtFiles.Items.Clear();
        fileCount.Text = "Нет файлов. Импортируйте .qznbt с ПК или выберите другую папку.";
        try
        {
            if (!Directory.Exists(directory.Text)) return;
            foreach (var path in Directory.EnumerateFiles(directory.Text).Where(p => p.EndsWith(".qznbt", StringComparison.OrdinalIgnoreCase) ||
                p.EndsWith(".cpenbt.json", StringComparison.OrdinalIgnoreCase)).OrderBy(Path.GetFileName).Take(2000))
                nbtFiles.Items.Add(Path.GetFileName(path));
            if (nbtFiles.Items.Count > 0) fileCount.Text = $"Файлов в библиотеке: {nbtFiles.Items.Count}";
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { ShowError(error); }
    }
    private void ChooseFolder()
    {
        if (running) return;
        using var dialog = new FolderBrowserDialog { Description = "Папка с NBT шалкеров", UseDescriptionForTitle = true, InitialDirectory = directory.Text };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        directory.Text = dialog.SelectedPath; RefreshFiles(); ScheduleConfig();
    }
    private void ImportFile()
    {
        using var dialog = new OpenFileDialog { Filter = "NBT шалкера|*.qznbt;*.cpenbt.json|Все файлы|*.*", CheckFileExists = true };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;
        try
        {
            var info = new FileInfo(dialog.FileName);
            if (info.Length < 8 || info.Length > 8 * 1024 * 1024 + 8192) throw new InvalidOperationException("Допустимый размер NBT — до 8 МиБ.");
            using var input = File.OpenRead(dialog.FileName);
            Span<byte> magic = stackalloc byte[8]; input.ReadExactly(magic);
            bool qz = magic.SequenceEqual("QZNBTF02"u8);
            if (!qz && !dialog.FileName.EndsWith(".cpenbt.json", StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("Нужен файл ПК QZNBTF02 (.qznbt) или .cpenbt.json. Обычные структуры .nbt — другой формат.");
            string name = SlotFromFile(Path.GetFileName(dialog.FileName));
            if (!AppSettings.ValidSlot(name))
            {
                name = slot.Text.Trim();
                if (!AppSettings.ValidSlot(name)) throw new InvalidOperationException("Укажите допустимое имя в поле «Имя слота» и повторите импорт.");
            }
            Directory.CreateDirectory(directory.Text);
            string alternate = Path.Combine(directory.Text, name + (qz ? ".cpenbt.json" : ".qznbt"));
            if (File.Exists(alternate))
            {
                // The shared core prefers JSON over QZ for equal slot names. Never silently load the other file.
                string prefix = name[..Math.Min(name.Length, 24)]; int suffix = 2;
                do { name = prefix + "_" + suffix++; }
                while (File.Exists(Path.Combine(directory.Text, name + ".qznbt")) || File.Exists(Path.Combine(directory.Text, name + ".cpenbt.json")));
            }
            string target = Path.Combine(directory.Text, name + (qz ? ".qznbt" : ".cpenbt.json"));
            if (!Path.GetFullPath(target).Equals(Path.GetFullPath(dialog.FileName), StringComparison.OrdinalIgnoreCase))
            {
                if (File.Exists(target) && MessageBox.Show(this, $"Заменить {Path.GetFileName(target)}?", "Импорт NBT", MessageBoxButtons.YesNo) != DialogResult.Yes) return;
                File.Copy(dialog.FileName, target, true);
            }
            RefreshFiles(); slot.Text = name; nbtFeedback.Text = "Импортирован: " + name + ". Нажмите «Включить NBT-крафт».";
        }
        catch (Exception error) { ShowError(error); }
    }
    private async Task NbtAction(string action)
    {
        if (busy) return;
        string name = slot.Text.Trim();
        if (!AppSettings.ValidSlot(name)) { ShowError(new InvalidOperationException("Имя: A–Z, 0–9, _ или -, до 32 символов.")); return; }
        if (action == "nbt.arm" && File.Exists(Path.Combine(directory.Text, name + ".qznbt")) && File.Exists(Path.Combine(directory.Text, name + ".cpenbt.json")))
        { ShowError(new InvalidOperationException("В папке два формата с этим именем. Переименуйте один файл, чтобы выбор NBT был однозначным.")); return; }
        if (action == "nbt.copy" && File.Exists(Path.Combine(directory.Text, name + ".cpenbt.json")) &&
            MessageBox.Show(this, $"Заменить сохранение {name}?", "Сохранение NBT", MessageBoxButtons.YesNo) != DialogResult.Yes) return;
        SetBusy(true);
        try { var result = await backend.Call(new { action, slot = name }); nbtFeedback.Text = result.GetString(); RefreshFiles(); }
        catch (Exception error) { ShowError(error); }
        finally { SetBusy(false); }
    }
    private void AddLog(string text)
    {
        AddLogs([text]);
    }
    private void AddLogs(IReadOnlyList<string> messages)
    {
        if (messages.Count == 0) return;
        var lines = new StringBuilder();
        if (messages.Count > 128) lines.AppendLine($"{DateTime.Now:HH:mm:ss} Пропущено старых событий: {messages.Count - 128}");
        foreach (var message in messages.Skip(Math.Max(0, messages.Count - 128)))
        {
            // Bound both text and work per UI tick; disk writes run on a bounded worker.
            if (lines.Length > 28000) { lines.AppendLine("… остальные события скрыты"); break; }
            var text = message.Length > 2000 ? message[..2000] : message;
            lines.AppendLine($"{DateTime.Now:HH:mm:ss} {text}");
        }
        if (log.TextLength > 64000) log.Text = log.Text[^32000..];
        string batch = lines.ToString();
        log.AppendText(batch);
        LogStore.Append(batch);
    }
    private void ShowError(Exception error)
    {
        if (closing || stopping || IsDisposed || error is OperationCanceledException) return;
        if (!preview) AddLog(error.Message);
        MessageBox.Show(this, error.Message, "CPE Relay", MessageBoxButtons.OK, MessageBoxIcon.Warning);
    }
    private void OpenShell(string target)
    {
        try { Process.Start(new ProcessStartInfo(target) { UseShellExecute = true }); }
        catch (Exception error) { ShowError(error); }
    }
    private async void OnClosing(object? sender, FormClosingEventArgs e)
    {
        if ((preview && !lifecycleTest) || closeAllowed) return;
        e.Cancel = true;
        if (closing) return;
        closing = true; ++lifecycle; pollTimer.Stop(); configTimer.Stop(); floating.Hide(); Hide(); Enabled = false;
        // A failed settings write must never skip shutdown, nor keep a window open.
        Task save = Task.CompletedTask;
        if (!preview)
        {
            ReadSettings();
            save = Task.Run(() => { try { settings.Save(); } catch (Exception error) { LogStore.Append("Настройки: " + error.Message); } });
        }
        try { await backend.StopAsync(); }
        catch (Exception error) { if (!preview) LogStore.Append("Остановка: " + error.Message); }
        finally
        {
            backend.Dispose();
            if (!preview) { try { await Task.WhenAll(save, LogStore.Finish()).WaitAsync(TimeSpan.FromMilliseconds(500)); } catch { } }
            closeAllowed = true; Close();
        }
    }
}
