// Static input validation in the real WebView2 runtime. This does not compare TWebFrame pixels.
using System;
using System.Collections;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.WinForms;

internal sealed class SmokeForm : Form
{
    [DllImport("user32.dll")] private static extern uint GetDpiForWindow(IntPtr hwnd);
    [DllImport("user32.dll")] private static extern bool SetProcessDpiAwarenessContext(IntPtr context);
    private readonly WebView2 browser = new WebView2();
    private readonly JavaScriptSerializer json = new JavaScriptSerializer { MaxJsonLength = Int32.MaxValue, RecursionLimit = 100 };
    private readonly string corpus, output, mode;
    private readonly HashSet<string> selected;
    private readonly List<object> records = new List<object>();
    private readonly List<object> failures = new List<object>();
    private int navigated, measurements, screenshots, attempted, planned;
    private string termination = "completed";
    private string runtime = "uninitialized";

    private const string Inspect = @"(()=>{try{
      const all=Array.from(document.querySelectorAll('*'));
      const probes=Array.from(document.querySelectorAll('[data-probe]'));
      const images=Array.from(document.images);
      const ids=all.filter(e=>e.id).map(e=>e.id);
      const rectangles=probes.map(e=>{const r=e.getBoundingClientRect();const s=getComputedStyle(e);return {
        id:e.id,tag:e.tagName,x:r.x,y:r.y,width:r.width,height:r.height,
        scrollWidth:e.scrollWidth,scrollHeight:e.scrollHeight,display:s.display,
        position:s.position,writingMode:s.writingMode,direction:s.direction,
        background:s.backgroundImage,opacity:s.opacity,transform:s.transform,
        clipPath:s.clipPath,gridTemplateColumns:s.gridTemplateColumns,
        overflowX:s.overflowX,overflowY:s.overflowY,fontFamily:s.fontFamily,
        queryHit:s.getPropertyValue('--query-hit'),subgridHit:s.getPropertyValue('--subgrid-hit')};});
      let rules=0,declarations=0,emptyStyleRules=0;
      const walk=rs=>{for(const r of rs){rules++;if(r.style){declarations+=r.style.length;if(!r.style.length)emptyStyleRules++;}if(r.cssRules)walk(r.cssRules);}};
      for(const sheet of document.styleSheets)walk(sheet.cssRules);
      const featureTests={subgrid:CSS.supports('grid-template-columns','subgrid'),container:CSS.supports('container-type','inline-size'),
        has:CSS.supports('selector(:has(>b))'),mask:CSS.supports('mask-image','linear-gradient(black,transparent)'),
        conic:CSS.supports('background-image','conic-gradient(red,blue)'),backdrop:CSS.supports('backdrop-filter','blur(1px)'),
        oklch:CSS.supports('color','oklch(60% .1 220)'),colorMix:CSS.supports('color','color-mix(in srgb,red,blue)'),
        containerUnits:CSS.supports('width','1cqi'),overflowClip:CSS.supports('overflow','clip')};
      return {id:document.querySelector('meta[name=fixture-id]').content,readyState:document.readyState,fontStatus:document.fonts.status,
        elementCount:all.length,probeCount:probes.length,uniqueIds:new Set(ids).size,idCount:ids.length,
        probeIds:probes.map(e=>e.id).sort(),scripts:document.scripts.length,styleSheets:document.styleSheets.length,
        imageCount:images.length,brokenImages:images.filter(e=>!e.complete||e.naturalWidth!==96||e.naturalHeight!==72).length,
        cssRules:rules,cssDeclarations:declarations,emptyStyleRules:emptyStyleRules,features:featureTests,
        viewport:[innerWidth,innerHeight],dpr:devicePixelRatio,scrollExtent:[document.documentElement.scrollWidth,document.documentElement.scrollHeight],
        finiteRectangles:rectangles.every(r=>[r.x,r.y,r.width,r.height].every(Number.isFinite)),rectangles:rectangles};
    }catch(error){return {inspectionError:String(error),stack:error.stack};}})()";

    private SmokeForm(Dictionary<string, object> input)
    {
        corpus = (string)input["corpus"];
        output = (string)input["output"];
        mode = (string)input["mode"];
        selected = new HashSet<string>(((IList)input["caseIds"]).Cast<string>());
        AutoScaleMode = AutoScaleMode.None;
        ClientSize = new Size(800, 600);
        Location = new Point(-30000, -30000);
        StartPosition = FormStartPosition.Manual;
        ShowInTaskbar = false;
        FormBorderStyle = FormBorderStyle.FixedToolWindow;
        browser.Dock = DockStyle.Fill;
        Controls.Add(browser);
        Shown += async delegate { await Run(); };
    }

    private Dictionary<string, object> ReadJson(string path)
    {
        return json.Deserialize<Dictionary<string, object>>(File.ReadAllText(path, Encoding.UTF8));
    }

    private string Hash(string path)
    {
        using (SHA256 hash = SHA256.Create())
        using (FileStream input = File.OpenRead(path))
            return BitConverter.ToString(hash.ComputeHash(input)).Replace("-", "").ToLowerInvariant();
    }

    private async Task<T> Bounded<T>(Task<T> task, string operation)
    {
        if (await Task.WhenAny(task, Task.Delay(20000)) != task)
            throw new TimeoutException(operation + " exceeded 20 seconds");
        return await task;
    }

    private async Task Navigate(string path)
    {
        var ready = new TaskCompletionSource<bool>();
        EventHandler<CoreWebView2NavigationCompletedEventArgs> handler = null;
        handler = delegate(object sender, CoreWebView2NavigationCompletedEventArgs e) {
            if (e.IsSuccess) ready.TrySetResult(true);
            else ready.TrySetException(new Exception("Navigation: " + e.WebErrorStatus));
        };
        browser.CoreWebView2.NavigationCompleted += handler;
        try {
            string relative = path.Substring(corpus.Length).TrimStart(Path.DirectorySeparatorChar).Replace(Path.DirectorySeparatorChar, '/');
            browser.CoreWebView2.Navigate("https://rendering.test/" + relative);
            await Bounded(ready.Task, "Navigation");
        }
        finally { browser.CoreWebView2.NavigationCompleted -= handler; }
    }

    private void Expect(bool condition, string message)
    {
        if (!condition) throw new Exception(message);
    }

    private async Task Measure(Dictionary<string, object> entry, int width, int height, bool capture)
    {
        ClientSize = new Size(width, height);
        await Task.Delay(capture ? 70 : 15);
        string raw = await Bounded(browser.CoreWebView2.ExecuteScriptAsync(Inspect), "Read-only measurement");
        var result = json.Deserialize<Dictionary<string, object>>(raw);
        Expect(result != null, "Read-only inspection returned null");
        string id = (string)entry["id"];
        string basename = id + "-" + width + "x" + height;
        File.WriteAllText(Path.Combine(output, basename + ".json"), raw, Encoding.UTF8);
        if (result.ContainsKey("inspectionError")) throw new Exception("Inspection: " + result["inspectionError"]);
        var complexity = (Dictionary<string, object>)entry["complexity"];
        Expect((string)result["id"] == id, "Wrong document loaded");
        Expect((string)result["fontStatus"] == "loaded", "Fonts are not ready");
        Expect(Convert.ToInt32(result["elementCount"]) == Convert.ToInt32(complexity["elements"]), "HTML5 DOM element count differs from source");
        Expect(Convert.ToInt32(result["probeCount"]) == Convert.ToInt32(complexity["trackedElements"]), "Probe count differs");
        var metadata = ReadJson(Path.Combine(corpus, ((string)entry["path"]).Replace('/', Path.DirectorySeparatorChar), "case.json"));
        string[] expectedProbes = ((IList)metadata["probeIds"]).Cast<string>().OrderBy(v => v, StringComparer.Ordinal).ToArray();
        string[] actualProbes = ((IList)result["probeIds"]).Cast<string>().OrderBy(v => v, StringComparer.Ordinal).ToArray();
        Expect(expectedProbes.SequenceEqual(actualProbes), "Probe identities differ from source metadata");
        Expect(Convert.ToInt32(result["uniqueIds"]) == Convert.ToInt32(result["idCount"]), "Duplicate DOM IDs");
        Expect(Convert.ToInt32(result["scripts"]) == 0 && !browser.CoreWebView2.Settings.IsScriptEnabled, "Fixture script policy violated");
        Expect(Convert.ToInt32(result["styleSheets"]) == 1, "Stylesheet missing");
        Expect(Convert.ToInt32(result["brokenImages"]) == 0, "Embedded PNG failed to decode");
        Expect(Convert.ToInt32(result["cssRules"]) >= 220, "CSS rule count unexpectedly small");
        Expect(Convert.ToInt32(result["cssDeclarations"]) >= 700, "CSS declaration count unexpectedly small");
        Expect(Convert.ToBoolean(result["finiteRectangles"]), "Non-finite layout rectangles");
        IList viewport = (IList)result["viewport"];
        Expect(Convert.ToInt32(viewport[0]) == width && Convert.ToInt32(viewport[1]) == height, "CSS viewport differs from requested size");
        if (capture) {
            string imagePath = Path.Combine(output, basename + ".png");
            using (FileStream image = File.Create(imagePath))
                await browser.CoreWebView2.CapturePreviewAsync(CoreWebView2CapturePreviewImageFormat.Png, image);
            using (Bitmap image = new Bitmap(imagePath)) {
                double dpr = Convert.ToDouble(result["dpr"]);
                Expect(image.Width == (int)Math.Round(width * dpr) && image.Height == (int)Math.Round(height * dpr), "PNG dimensions differ from CSS viewport and DPR");
            }
            screenshots++;
        }
        measurements++;
        records.Add(new { id = id, viewport = new[] { width, height }, elements = result["elementCount"], rules = result["cssRules"], declarations = result["cssDeclarations"], imageCount = result["imageCount"], captured = capture, inspectionSha256 = Hash(Path.Combine(output, basename + ".json")) });
    }

    private async Task Run()
    {
        try {
            var env = await CoreWebView2Environment.CreateAsync(null, Path.Combine(output, "profile"), null);
            await browser.EnsureCoreWebView2Async(env);
            runtime = browser.CoreWebView2.Environment.BrowserVersionString;
            browser.ZoomFactor = 1.0;
            browser.CoreWebView2.Settings.IsScriptEnabled = false;
            browser.CoreWebView2.Settings.IsWebMessageEnabled = false;
            browser.CoreWebView2.Settings.AreDefaultContextMenusEnabled = false;
            browser.CoreWebView2.Settings.IsStatusBarEnabled = false;
            browser.CoreWebView2.SetVirtualHostNameToFolderMapping("rendering.test", corpus, CoreWebView2HostResourceAccessKind.DenyCors);
            var manifest = ReadJson(Path.Combine(corpus, "manifest.json"));
            IList entries = (IList)manifest["cases"];
            var knownIds = new HashSet<string>(entries.Cast<Dictionary<string, object>>().Select(e => (string)e["id"]));
            Expect(selected.All(knownIds.Contains), "Unknown selected case ID");
            planned = mode == "all" ? entries.Count : mode == "selected" ? selected.Count : Math.Min(40, entries.Count);
            for (int i = 0; i < entries.Count; i++) {
                bool sample = i < 20 || i >= entries.Count - 20;
                var entry = (Dictionary<string, object>)entries[i];
                string id = (string)entry["id"];
                if (mode == "selected") { if (!selected.Contains(id)) continue; sample = true; }
                else if (mode != "all" && !sample) continue;
                attempted++;
                try {
                    string folder = Path.Combine(corpus, ((string)entry["path"]).Replace('/', Path.DirectorySeparatorChar));
                    Expect(Hash(Path.Combine(folder, "index.html")) == (string)entry["htmlSha256"], "HTML input hash changed");
                    Expect(Hash(Path.Combine(folder, "style.css")) == (string)entry["cssSha256"], "CSS input hash changed");
                    ClientSize = new Size(800, 600);
                    await Navigate(Path.Combine(folder, "index.html"));
                    navigated++;
                    await Measure(entry, 800, 600, sample);
                    if (sample) {
                        await Measure(entry, 384, 768, true);
                        await Measure(entry, 1280, 800, true);
                    }
                }
                catch (Exception error) {
                    failures.Add(new { id = id, error = error.ToString() });
                    File.WriteAllText(Path.Combine(output, "errors.json"), json.Serialize(failures), Encoding.UTF8);
                    // A stuck renderer contaminates later navigations. Preserve the first timeout and stop.
                    if (error is TimeoutException) { termination = "renderer-timeout"; break; }
                }
                if (failures.Count >= 5) { termination = "five-input-or-inspection-failures"; break; }
                if (navigated % 20 == 0)
                    File.WriteAllText(Path.Combine(output, "progress.txt"), "WebView2 " + navigated + "/" + planned + " cases; failures=" + failures.Count, Encoding.UTF8);
            }
            var summary = new {
                schemaVersion = 1, runtimeVersion = runtime, mode = mode, output = output,
                navigatedCases = navigated, viewportMeasurements = measurements, screenshots = screenshots,
                plannedCases = planned, attemptedCases = attempted, notRunCases = planned - attempted, terminationReason = termination,
                failures = failures.Count, errors = failures, actualWindowDpi = GetDpiForWindow(Handle),
                pageScriptsEnabled = false, domMutation = false, zoomFactor = browser.ZoomFactor,
                corpusManifestSha256 = Hash(Path.Combine(corpus, "manifest.json")),
                sdkCoreSha256 = Hash(typeof(CoreWebView2).Assembly.Location),
                validation = "HTML5 DOM counts/unique IDs, CSSOM rule/declaration counts, PNG decode, finite layout rectangles, viewport and screenshot dimensions",
                renderingComparison = "not-run; WebView2 input smoke validation only", records = records
            };
            File.WriteAllText(Path.Combine(output, "summary.json"), json.Serialize(summary), Encoding.UTF8);
            Environment.ExitCode = failures.Count == 0 ? 0 : 1;
        }
        catch (Exception error) {
            File.WriteAllText(Path.Combine(output, "fatal.txt"), error.ToString(), Encoding.UTF8);
            Environment.ExitCode = 1;
        }
        finally { browser.Dispose(); Close(); }
    }

    [STAThread]
    private static void Main(string[] args)
    {
        if (args.Length != 1) { Environment.ExitCode = 2; return; }
        SetProcessDpiAwarenessContext(new IntPtr(-4));
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        var serializer = new JavaScriptSerializer();
        var input = serializer.Deserialize<Dictionary<string, object>>(File.ReadAllText(args[0], Encoding.UTF8));
        Application.Run(new SmokeForm(input));
    }
}
