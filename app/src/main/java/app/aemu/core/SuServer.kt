package app.aemu.core

import android.content.Context
import android.net.LocalSocket
import android.system.Os
import org.json.JSONObject
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * Root for the guest. The guest's user ids are aemu64's to give (run64.c: setuid always succeeds), so becoming root
 * is a matter of asking: /system/xbin/su (native/aemusu) asks here over /dev/socket/aemu_su whether the calling app
 * may, and runs a root shell when the answer is yes.
 *
 * Request (text, ends with an empty line): "AEMUSU 1", "uid=", "pid=", "ppid=", "cmd=". Answer: "1\n" or "0\n".
 * Decisions remembered per app live in the image directory (su.json); [forget] clears them.
 */
class SuServer(private val paths: VmPaths, private val log: (String) -> Unit) {

    data class Request(val uid: Int, val pid: Int, val app: String, val cmd: String)

    /** Puts the question to the person; [answer] is called once with (allow, remember). Null: nobody to ask. */
    @Volatile var onRequest: ((Request, (Boolean, Boolean) -> Unit) -> Unit)? = null
    /** A request refused because nobody could be asked (the VM screen was not up). */
    @Volatile var onUnanswered: ((Request) -> Unit)? = null

    private val server = UnixServer(paths.socket(SOCKET), "su") { c -> serve(c) }

    fun start() = server.start(log)
    fun stop() = server.stop()

    private fun serve(c: LocalSocket) {
        c.soTimeout = 5000
        val kv = HashMap<String, String>()
        val r = c.inputStream.bufferedReader()
        val head = r.readLine() ?: return
        if (!head.startsWith("AEMUSU")) return
        while (true) {
            val l = r.readLine() ?: break
            if (l.isEmpty()) break
            val i = l.indexOf('=')
            if (i > 0) kv[l.substring(0, i)] = l.substring(i + 1)
        }
        val uid = kv["uid"]?.toIntOrNull() ?: return
        val pid = kv["pid"]?.toIntOrNull() ?: 0
        val ppid = kv["ppid"]?.toIntOrNull() ?: 0
        val req = Request(uid, pid, appName(uid, ppid), kv["cmd"].orEmpty())
        val key = policyKey(req)
        val remembered = synchronized(lock) { load().optString(key, "").takeIf { it.isNotEmpty() } }
        val allow = when (remembered) {
            "allow" -> true
            "deny" -> false
            else -> ask(req, key)
        }
        log("su: ${req.app} (uid $uid) ${if (allow) "granted" else "denied"}${if (remembered != null) " (remembered)" else ""}" +
            (if (req.cmd.isNotEmpty()) ": ${req.cmd.take(120)}" else ""))
        runCatching { c.outputStream.write(if (allow) "1\n".toByteArray() else "0\n".toByteArray()); c.outputStream.flush() }
    }

    private fun ask(req: Request, key: String): Boolean {
        val h = onRequest ?: run { onUnanswered?.invoke(req); return false }
        val done = CountDownLatch(1)
        val result = java.util.concurrent.atomic.AtomicBoolean(false)
        h(req) { allow, remember ->
            if (done.count > 0) {
                result.set(allow)
                if (remember) synchronized(lock) { save(load().put(key, if (allow) "allow" else "deny")) }
                done.countDown()
            }
        }
        // the prompt waits for the person; su waits as long (an unanswered question is a no)
        if (!done.await(ASK_TIMEOUT_S, TimeUnit.SECONDS)) return false
        return result.get()
    }

    /** The app behind a uid: its package names from the guest's packages.list, else the calling process' name. */
    private fun appName(uid: Int, ppid: Int): String {
        when (uid) {
            0 -> return "root"
            1000 -> return "system"
            2000 -> return "shell"
        }
        val appId = uid % 100000
        val names = runCatching {
            File(paths.root, "data/system/packages.list").readLines().mapNotNull { l ->
                val p = l.split(' ')
                if (p.size >= 2 && p[1].toIntOrNull() == appId) p[0] else null
            }
        }.getOrDefault(emptyList())
        if (names.isNotEmpty()) return names.joinToString(", ")
        val proc = runCatching {
            File("/proc/$ppid/cmdline").readBytes().takeWhile { it != 0.toByte() }.toByteArray().toString(Charsets.UTF_8)
        }.getOrDefault("")
        return proc.ifBlank { "uid $uid" }
    }

    private fun policyKey(req: Request) = if (req.app.startsWith("uid ")) "uid:${req.uid}" else req.app

    private val lock = Any()
    private val policyFile get() = File(paths.dir, POLICY)
    private fun load(): JSONObject = runCatching { JSONObject(policyFile.readText()) }.getOrDefault(JSONObject())
    private fun save(o: JSONObject) { runCatching { policyFile.writeText(o.toString(1)) } }

    companion object {
        const val SOCKET = "aemu_su"
        private const val POLICY = "su.json"
        private const val ASK_TIMEOUT_S = 60L
        private const val ASSET = "engines/common/aemu_su"

        /** Forgets every remembered decision of this image. */
        fun forget(paths: VmPaths) { File(paths.dir, POLICY).delete() }

        /**
         * Puts su into the tree as /system/xbin/su (on), or takes ours away again (off). A su the firmware came with
         * is parked beside the other parked files while ours is there, and put back after.
         */
        fun install(ctx: Context, paths: VmPaths, on: Boolean, log: (String) -> Unit) {
            val su = File(paths.root, "system/xbin/su")
            val mark = File(paths.root, "system/xbin/.aemu-su")
            val parked = File(paths.root, "system/.aemu-parked/system#xbin#su")
            runCatching {
                if (on) {
                    val size = EngineAssets.size(ctx, ASSET)
                    if (size < 0) { log("su: $ASSET missing from the app"); return }
                    if (mark.isFile && su.isFile && su.length() == size && mark.readText().trim() == size.toString()) return
                    su.parentFile?.mkdirs()
                    if (!mark.isFile && (su.exists() || isLink(su))) {
                        parked.parentFile?.mkdirs()
                        if (isLink(su)) Os.remove(su.absolutePath) else if (!parked.exists()) su.renameTo(parked) else su.delete()
                    }
                    EngineAssets.open(ctx, ASSET).use { i -> su.outputStream().use { o -> i.copyTo(o) } }
                    su.setReadable(true, false); su.setExecutable(true, false)
                    mark.writeText(size.toString())
                    log("su: installed /system/xbin/su")
                } else if (mark.isFile) {
                    su.delete(); mark.delete()
                    if (parked.isFile) parked.renameTo(su)
                    log("su: removed /system/xbin/su")
                }
            }.onFailure { log("su: ${if (on) "install" else "removal"} failed: ${it.message}") }
        }

        private fun isLink(f: File) = runCatching { android.system.OsConstants.S_ISLNK(Os.lstat(f.absolutePath).st_mode) }.getOrDefault(false)
    }
}
