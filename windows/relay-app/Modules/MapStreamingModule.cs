using System.Text.RegularExpressions;
namespace CpeRelay.Windows;

internal sealed class MapStreamingModule : RelayModule
{
    private readonly Label state = Note("Ожидание подключения Minecraft.");
    private readonly Label progress = Note("Статистика появится при загрузке карт.");
    private bool connected;
    internal MapStreamingModule() : base("map-streaming", "Загрузка карт", "Постепенная загрузка с сервера. Всегда включена.")
    {
        Card(Caption("РАБОТАЕТ БЕЗ КНОПКИ"), state,
            Note("После входа карты загружаются автоматически. Ничего включать вручную не нужно.\nОбычные игровые пакеты имеют приоритет; карты поступают по одной."));
        Card(Caption("ДО 4000 КАРТ"), progress,
            Note("Темп: до 8 обновлений и 512 КиБ в секунду до сжатия.\nПамять очереди: до 8 МиБ. Временное хранилище: до 512 МиБ."),
            Note("Отправлено — это обновления, переданные реле, а не подтверждение отрисовки.\nСкорость зависит от размера карт, сети и ответов сервера."));
        Card(Caption("НЕ ПУТАТЬ С ZIP-КРАФТОМ"), Note("Создание карт из архива находится в отдельном модуле «Карты из ZIP».\nЕго запуск не требуется для просмотра карт в мире."));
    }
    internal void UpdateConnection(bool running, bool ready)
    {
        if (connected && !ready) progress.Text = "Сеанс завершён. При следующем входе начнётся новая очередь.";
        connected = ready;
        state.Text = ready ? "Подключено · автоматическая загрузка активна" : running ? "Реле запущено · ожидание Minecraft" : "Реле остановлено · загрузка начнётся после подключения";
        state.ForeColor = ready ? Theme.Green : Theme.Muted;
    }
    internal void Observe(string line)
    {
        if (!line.Contains("map_scheduler intercepted=")) return;
        string Value(string key) => Regex.Match(line, @"\b" + key + @"=(\d+)\b", RegexOptions.CultureInvariant).Groups[1].Value;
        progress.Text = $"Получено обновлений: {Value("intercepted")}  ·  Отправлено: {Value("sent")}\n" +
            $"Ожидают запроса: {Value("requestsPending")}  ·  Готовятся к отправке: {Value("pending")}";
    }
}
