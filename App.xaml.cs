using System.Threading;
using System.Windows;

namespace S4W
{
    public partial class App : System.Windows.Application
    {
        private static Mutex? _mutex;

        protected override void OnStartup(StartupEventArgs e)
        {
            const string mutexName = "S4W_SingleInstance_7F2A";
            _mutex = new Mutex(true, mutexName, out bool createdNew);

            if (!createdNew)
            {
                System.Windows.MessageBox.Show(
                    "S4W is already running.", "S4W",
                    MessageBoxButton.OK, MessageBoxImage.Information);
                Shutdown();
                return;
            }

            base.OnStartup(e);
        }

        protected override void OnExit(ExitEventArgs e)
        {
            _mutex?.ReleaseMutex();
            _mutex?.Dispose();
            base.OnExit(e);
        }
    }
}
