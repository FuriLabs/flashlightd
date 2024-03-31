using Gst;
using GLib;

[DBus (name = "org.droidian.Flashlightd")]
public class FlashlightServer : GLib.Object {
    public static bool is_exynos = false;

    public static List<string> sysfs_path;

    private const size_t sysfs_exynos_size = 1;
    private string[] sysfs_exynos = {"/sys/devices/virtual/camera/flash/rear_flash"};

    public const string EXYNOS_SYSFS_ENABLE = "1";
    public const string EXYNOS_SYSFS_DISABLE = "0";

    public int Brightness { get; set; }

    private weak DBusConnection conn;
    private Gst.Element pipeline;

    public FlashlightServer (DBusConnection conn) {
        this.conn = conn;
        this.notify.connect (send_property_change);
    }

    public static void initialize() {
        sysfs_path = new List<string>();
        sysfs_path.append("/sys/class/leds/torch-light/brightness");
        sysfs_path.append("/sys/class/leds/flashlight/brightness");
        sysfs_path.append("/sys/class/leds/torch-light0/brightness");
        sysfs_path.append("/sys/class/leds/torch-light1/brightness");
        sysfs_path.append("/sys/class/leds/led:flash_torch/brightness");
        sysfs_path.append("/sys/class/leds/led:flash_0/brightness");
        sysfs_path.append("/sys/class/leds/led:flash_1/brightness");
        sysfs_path.append("/sys/class/leds/led:flash_2/brightness");
        sysfs_path.append("/sys/class/leds/led:flash_3/brightness");
        sysfs_path.append("/sys/class/leds/led:torch_0/brightness");
        sysfs_path.append("/sys/class/leds/led:torch_1/brightness");
        sysfs_path.append("/sys/class/leds/led:torch_2/brightness");
        sysfs_path.append("/sys/class/leds/led:torch_3/brightness");
        sysfs_path.append("/sys/class/leds/led:switch/brightness");
        sysfs_path.append("/sys/class/leds/led:switch_0/brightness");
        sysfs_path.append("/sys/class/leds/led:switch_1/brightness");
        sysfs_path.append("/sys/class/leds/led:switch_2/brightness");
        sysfs_path.append("/sys/devices/platform/soc/soc:i2c@1/i2c-23/23-0059/s2mpb02-led/leds/torch-sec1/brightness");
        sysfs_path.append("/sys/class/flashlight_core/flashlight/flashlight_torch");
    }

    public void SetBrightness(uint bvalue) throws GLib.Error {
        // mimic logind SetBrightness
        Brightness = (int) bvalue;
    }

    public static bool sysfs = false;

    Gst.StateChangeReturn result;

    private void set_flashlight() {
        // exynos devices cannot use our gst-droid plugin with droidcamsrc sink (at least not on droidian) and they use different values and different paths for flashlight in sysfs.
        // as a result cannot use the fallack sysfs backend or the gstreamer stuff so lets check if device is exynos and act accordingly.
        if (is_exynos) {
            foreach (var path in sysfs_exynos) {
                var file = File.new_for_path(path);
                if (file.query_exists()) {
                    try {
                        var out_stream = file.replace(null, false, FileCreateFlags.NONE, null);
                        out_stream.write_all((Brightness > 0) ? EXYNOS_SYSFS_ENABLE.data : EXYNOS_SYSFS_DISABLE.data, null);
                        out_stream.close();
                    } catch (Error e) {
                        // some paths might throw an error because of permissions we just want to ignore those
                    }
                }
            }
        } else {
            if (!sysfs) {
                if (Brightness == 0 && pipeline != null) {
                    pipeline.set_state (State.NULL);
                    pipeline = null;
                    return;
                } else if (Brightness > 0) {
                    // turn on flashlight
                    try {
                        pipeline = Gst.parse_launch("droidcamsrc video-torch=true mode=2 ! fakesink");
                        result = pipeline.set_state (State.PLAYING);
                    } catch (Error e) {
                        sysfs = true;
                    }
                }
            }

            // fallback to sysfs if droidcamsrc isn't available
            if (result == StateChangeReturn.FAILURE || sysfs) {
                sysfs = true;

                foreach (var path in sysfs_path) {
                    var file = File.new_for_path(path);
                    if (file.query_exists()) {
                        try {
                            var out_stream = file.replace(null, false, FileCreateFlags.NONE, null);
                            string brightnessStr = (Brightness > 0 ? "1" : "0") + "\n";
                            out_stream.write_all(brightnessStr.data, null);
                            out_stream.flush();
                            out_stream.close();
                        } catch (Error e) {
                            // some paths might throw an error because of permissions we just want to ignore those
                        }
                    }
                }
            }
        }
    }

    private void send_property_change (ParamSpec p) {
        var builder = new VariantBuilder (VariantType.ARRAY);
        var invalid_builder = new VariantBuilder (new VariantType ("as"));

        if (p.name == "Brightness") {
            Variant i = Brightness;
            builder.add ("{sv}", "Brightness", i);

            set_flashlight();
        }

        try {
            conn.emit_signal (null,
                              "/org/droidian/Flashlightd",
                              "org.freedesktop.DBus.Properties",
                              "PropertiesChanged",
                              new Variant ("(sa{sv}as)",
                                           "org.droidian.Flashlightd",
                                           builder,
                                           invalid_builder)
                              );
        } catch (Error e) {
            stderr.printf ("%s\n", e.message);
        }
    }
}

[DBus (name = "org.droidian.Flashlightd")]
public errordomain FlashlightError {
    SOME_ERROR
}

void on_bus_aquired (DBusConnection conn) {
    try {

        conn.register_object ("/org/droidian/Flashlightd",
                              new FlashlightServer (conn));
    } catch (IOError e) {
        stderr.printf ("Could not register service\n");
    }
}

void main (string[] args) {
    uint8[] content;
    string etag_out;
    string device_model_file = "/proc/device-tree/model";
    string flashlight_sysfs_file = "/usr/lib/droidian/device/flashlightd-sysfs";
    string custom_sysfs_nodes_file = "/usr/lib/droidian/device/flashlightd-sysfs-nodes";

    try {
        if (FileUtils.test (device_model_file, FileTest.EXISTS)) {
            File file = File.new_for_path (device_model_file);
            file.load_contents (null, out content, out etag_out);
            var raw_content = (string) content;
            if (raw_content.contains ("EXYNOS")) {
                FlashlightServer.is_exynos = true;
            }
        }

        if (FileUtils.test (flashlight_sysfs_file, FileTest.EXISTS)) {
            FlashlightServer.sysfs = true;
        }
    } catch (Error e) {
        // permission issue?
    }

    if (!FlashlightServer.is_exynos) {
        // Initialize GStreamer
        Gst.init (ref args);
    }

    FlashlightServer.initialize();

    try {
        if (FileUtils.test (custom_sysfs_nodes_file, FileTest.EXISTS)) {
            File file = File.new_for_path (custom_sysfs_nodes_file);
            file.load_contents (null, out content, out etag_out);
            var raw_content = (string) content;
            string[] new_nodes = raw_content.split(",");
            foreach (string node in new_nodes) {
                string trimmed_node = node.strip();

                if (trimmed_node != "") {
                    FlashlightServer.sysfs_path.append(node.strip());
                }
            }
        }
    } catch (Error e) {
        // permission issue?
    }

    GLib.Bus.own_name (BusType.SESSION, "org.droidian.Flashlightd", BusNameOwnerFlags.NONE,
                  on_bus_aquired,
                  () => {},
                  () => stderr.printf ("Could not aquire name\n"));

    new MainLoop ().run ();
}
