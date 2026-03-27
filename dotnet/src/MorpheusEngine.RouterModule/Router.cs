using System.Net;
using System.Text.Json;
using System.Text;

namespace MorpheusEngine
{
    public class Router
    {
        public const int PORT = 8790;

        private HttpListener _listener = new HttpListener();
        private bool _shutdownRequested = false;

        public void RequestShutdown() => _shutdownRequested = true;

        public async Task Run()
        {
            Initialize();

            try
            {
                while (!_shutdownRequested)
                {
                    HttpListenerContext context = await _listener.GetContextAsync();
                    ProcessQuery(context);
                }
            }
            catch (HttpListenerException e)
            {
                Console.WriteLine("Error encountered: " + e.Message);
            }
            finally
            {
                Shutdown();
            }
        }

        private void Initialize()
        {
            _listener.Prefixes.Add($"http://127.0.0.1:{PORT}/");
            _listener.Start();
            Console.WriteLine($"Router module listening on http://127.0.0.1:{PORT}/");

            Console.WriteLine("Router initialized.");
        }
        private void ProcessQuery(HttpListenerContext context)
        {
            if (context.Request.Url == null)
            {
                return;
            }

            var path = context.Request.Url.AbsolutePath;

            Console.WriteLine("Router received call: " + path);

            if (path.Equals("/info", StringComparison.OrdinalIgnoreCase))
            {
                ProcessRequest_info(context);
                return;
            }

            Console.WriteLine("Request for router did not match any expected endpoints. Returning 404.");
            Respond(context, 404, new { ok = false, error = "Not found: " + path});
        }
        private void Shutdown()
        {
            _listener.Stop();
            _listener.Close();

            Console.WriteLine("Router shut down.");
        }

        private void Respond(HttpListenerContext context, int statusCode, object payload)
        {
            var response = context.Response;
            response.StatusCode = statusCode;
            response.ContentType = "application/json";
            var bytes = Encoding.UTF8.GetBytes(JsonSerializer.Serialize(payload));
            response.ContentLength64 = bytes.LongLength;
            response.OutputStream.Write(bytes);
            response.OutputStream.Close();
        }

        private void ProcessRequest_info(HttpListenerContext context)
        {
            using var reader = new StreamReader(context.Request.InputStream, context.Request.ContentEncoding);
            var body = reader.ReadToEnd();

            Console.WriteLine("Router.ProcessRequest_info(): responding with 200.");
            Respond(context, 200, new { ok = true, module_name = "router" });
        }
    }
}