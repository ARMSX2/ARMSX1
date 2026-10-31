package com.armsx2.companion

import android.app.Activity
import android.app.Presentation
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Bitmap
import android.graphics.RectF
import android.graphics.Typeface
import android.graphics.LinearGradient
import android.graphics.Shader
import android.hardware.display.DisplayManager
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.Display
import android.view.View
import android.view.WindowManager
import kr.co.iefriends.pcsx2.NativeApp
import com.armsx2.BuildConfig
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.min

private fun hudArtworkReady(images: Map<Int, Bitmap>): Boolean =
    images[CtrPortraitReader.WUMPA] != null && "0123456789:./TIME LAPX".all {
        it == ' ' || (images[CtrPortraitReader.glyphKey(it, false)] != null &&
            images[CtrPortraitReader.glyphKey(it, true)] != null)
    }

/** Secondary display is UI only; never creates another emulation Surface/native VM. */
class CtrCompanion(private val activity: Activity) : DisplayManager.DisplayListener {
    private val displays = activity.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager
    private val main = Handler(Looper.getMainLooper())
    private val worker = Executors.newSingleThreadScheduledExecutor()
    private var task: ScheduledFuture<*>? = null
    private var presentation: Presentation? = null
    private var panel: RacePanel? = null
    private var resumed = false
    private var closed = false
    private var displayId: Int? = null
    @Volatile private var generation = 0

    fun resume() {
        if (resumed || closed) return
        resumed = true
        displays.registerDisplayListener(this, main)
        refresh()
    }
    fun pause() {
        if (!resumed) return
        resumed = false
        displays.unregisterDisplayListener(this)
        clear()
    }
    fun close() {
        if (closed) return
        closed = true
        pause()
        worker.shutdownNow()
    }
    private fun clear() {
        runCatching { NativeApp.setSwanCtrHudMoved(false) }
        generation++
        task?.cancel(false)
        task = null
        main.removeCallbacksAndMessages(null)
        presentation?.dismiss()
        presentation = null
        panel = null
        displayId = null
    }
    private fun refresh() {
        if (!resumed || closed || activity.isFinishing || activity.isDestroyed) return
        val display = displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION)
            .firstOrNull { it.displayId != activity.window.decorView.display?.displayId }
        // Keep one sampler per foreground Activity; detached displays stop all RAM polling.
        if (display == null) { clear(); return }
        if (task != null && displayId == display.displayId) return
        clear()
        displayId = display.displayId
        val token = ++generation
        // Do not build an unbounded UI backlog if the main thread is busy.
        val uiPending = AtomicBoolean(false)
        var lastDiagnostic = ""
        var lastDiagnosticAt = 0L
        var portraits: Map<Int, Bitmap> = emptyMap()
        var portraitTrack = ""
        var nextPortraitAt = 0L
        var nextSampleAt = 0L
        var lastSerial = ""
        var lastRace: CtrRaceReader.Race? = null
        task = worker.scheduleWithFixedDelay(sample@{
            if (token != generation || uiPending.get()) return@sample
            val serial = runCatching { NativeApp.getGameSerial() }.getOrDefault("")
            val now = SystemClock.uptimeMillis()
            if(serial != lastSerial) { lastSerial=serial;lastRace=null;nextSampleAt=0 }
            if(now<nextSampleAt)return@sample
            var snapshotBytes = 0
            var snapshotFailure = ""
            var screen = CtrScreenState.read(serial,null)
            var hub: CtrHubReader.Hub? = null
            val result = if (token == generation && CtrRaceReader.supported(serial)) {
                runCatching {
                    val ram = NativeApp.getSwanMemorySnapshot()
                    snapshotBytes = ram?.size ?: 0
                    screen = CtrScreenState.read(serial,ram)
                    hub = CtrHubReader.read(serial,ram)
                    CtrRaceReader.diagnose(serial, ram)
                }.onFailure { snapshotFailure = it.javaClass.simpleName }.getOrNull()
            } else null
            val liveRace = if(screen.kind==CtrScreenState.Kind.RACE) result?.race else null
            if(liveRace!=null)lastRace=liveRace
            if(screen.kind==CtrScreenState.Kind.RESULTS && result?.race!=null)lastRace=result.race
            val raceSummary=lastRace
            val liveHub=hub
            nextSampleAt=now+if(liveRace!=null||liveHub!=null)100 else 500
            val artKey=liveRace?.let { "race:${it.track}" } ?: liveHub?.let { "hub:${it.identity}" } ?: ""
            if (artKey.isEmpty()) {
                portraits = emptyMap()
                portraitTrack = ""
                nextPortraitAt = 0L
            } else {
                if (portraitTrack != artKey) {
                    portraits = emptyMap()
                    portraitTrack = artKey
                    nextPortraitAt = 0L
                }
                val raceArtMissing=liveRace!=null &&
                    (liveRace.racers.any { portraits[it.character] == null } || !hudArtworkReady(portraits) ||
                        (liveRace.hud.itemIcon() >= 0 && portraits[100+liveRace.hud.itemIcon()]==null))
                val hubArtMissing=liveHub!=null && (portraits[liveHub.character]==null ||
                    portraits[CtrPortraitReader.HUB_BOTTOM]==null ||
                    (liveHub.showTop && portraits[CtrPortraitReader.HUB_TOP]==null))
                if(token==generation && now>=nextPortraitAt && (raceArtMissing||hubArtMissing)) {
                    nextPortraitAt = now + 3000L
                    runCatching {
                        val capture = NativeApp.getSwanCompanionSnapshot()
                        val capturedRace = CtrRaceReader.read(serial, capture)
                        val capturedHub = CtrHubReader.read(serial,capture)
                        if ((liveRace!=null && capturedRace?.track == liveRace.track) ||
                            (liveHub!=null && capturedHub?.identity == liveHub.identity)) {
                            val decoded = CtrPortraitReader.read(serial, capture).associate {
                                it.character to Bitmap.createBitmap(it.pixels, it.width, it.height, Bitmap.Config.ARGB_8888)
                            }
                            portraits = portraits + decoded
                        }
                    }.onFailure { snapshotFailure = "Portraits: ${it.javaClass.simpleName}" }
                }
            }
            val decodedPortraits = portraits
            val diagnostic = if(liveHub!=null) "HUB: ${liveHub.name} · ${liveHub.identity} · Map images: ${portraits[4000]!=null}/${portraits[4001]!=null}"
            else if(liveRace==null) "SCREEN: ${screen.kind} · ${result?.code ?: snapshotFailure} · Serial: $serial"
            else if (result != null)
                "${result.code}: ${result.reason}\n${result.detail}\nSerial: $serial · RAM: $snapshotBytes bytes\nPortraits: ${portraits.keys.count { it < 16 }}/16 · HUD art: ${hudArtworkReady(portraits)} · Item: ${liveRace?.hud?.itemIcon()} ${snapshotFailure}\n${runCatching { NativeApp.getSwanCtrHudStatus() }.getOrDefault("HUD status unavailable")}"
            else "Snapshot unavailable: ${snapshotFailure.ifBlank { "not a supported CTR game" }}\nSerial: $serial"
            if (BuildConfig.DEBUG && CtrRaceReader.isCtr(serial) &&
                (lastDiagnostic.isEmpty() || now - lastDiagnosticAt >= 2000L) &&
                (diagnostic != lastDiagnostic || now - lastDiagnosticAt >= 10000L)) {
                Log.i("CTR-Companion", diagnostic.replace('\n', ' '))
                lastDiagnostic = diagnostic
                lastDiagnosticAt = now
            }
            val stillSameGame = runCatching { NativeApp.getGameSerial() == serial }.getOrDefault(false)
            if (token != generation || !uiPending.compareAndSet(false, true)) return@sample
            main.post {
                try {
                    if (!stillSameGame || !resumed || token != generation ||
                        runCatching { NativeApp.getGameSerial() != serial }.getOrDefault(true)) return@post
                    if (!CtrRaceReader.isCtr(serial)) {
                        presentation?.dismiss(); presentation = null; panel = null
                    } else {
                        if (presentation == null) show(display)
                        panel?.race = liveRace
                        panel?.hub = liveHub
                        panel?.screen = screen
                        panel?.lastRace = raceSummary
                        panel?.portraits = decodedPortraits
                        panel?.message = if (CtrRaceReader.supported(serial))
                            "Waiting for a race…" else "CTR companion: US version (SCUS-94426) required"
                        panel?.diagnostic = diagnostic
                        // Never hide the game's HUD unless a real second-screen race panel exists.
                        NativeApp.setSwanCtrHudMoved(presentation?.isShowing == true && liveRace != null &&
                            liveRace.hud.totalLaps in 1..7 && liveRace.hud.wumpa in 0..10 &&
                            liveRace.hud.item in 0..16 &&
                            hudArtworkReady(decodedPortraits) && panel?.hasDrawnArtwork == true &&
                            liveRace.racers.all { decodedPortraits[it.character] != null } &&
                            (liveRace.hud.itemIcon()<0 || decodedPortraits[100+liveRace.hud.itemIcon()]!=null))
                        panel?.invalidate()
                    }
                } finally { uiPending.set(false) }
            }
        }, 0, 100, TimeUnit.MILLISECONDS)
    }
    private fun show(display: Display) {
        if (!display.isValid || activity.isFinishing || activity.isDestroyed) return
        var createdPanel: RacePanel? = null
        val next = object : Presentation(activity, display) {
            override fun onCreate(state: android.os.Bundle?) {
                super.onCreate(state)
                window?.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON or
                    WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE)
                @Suppress("DEPRECATION")
                window?.decorView?.systemUiVisibility = View.SYSTEM_UI_FLAG_FULLSCREEN or
                    View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                val view = RacePanel(context)
                createdPanel = view
                setContentView(view)
            }
        }
        try { next.show(); presentation = next; panel = createdPanel }
        catch (_: WindowManager.InvalidDisplayException) { next.dismiss() }
    }
    override fun onDisplayAdded(displayId: Int) = refresh()
    override fun onDisplayRemoved(displayId: Int) {
        if (displayId == this.displayId) clear()
        refresh()
    }
    override fun onDisplayChanged(displayId: Int) {
        if (displayId == this.displayId || displayId == activity.window.decorView.display?.displayId) {
            clear(); refresh()
        }
    }
}

private class RacePanel(context: Context) : View(context) {
    var race: CtrRaceReader.Race? = null
    var hub: CtrHubReader.Hub? = null
    var screen = CtrScreenState.read("SCUS-94426",null)
    var lastRace: CtrRaceReader.Race? = null
    var message = "Waiting for a race…"
    var diagnostic = "Checking race data…"
    var portraits: Map<Int, Bitmap> = emptyMap()
    var hasDrawnArtwork = false
        private set
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val glyphTints = HashMap<Pair<Bitmap, Boolean>, Bitmap>()
    private var tintSource: Map<Int, Bitmap>? = null
    private val colors = intArrayOf(0xffffa726.toInt(), 0xffab47bc.toInt(), 0xffef5350.toInt(),
        0xffffee58.toInt(), 0xff42a5f5.toInt(), 0xff66bb6a.toInt(), 0xff26c6da.toInt(), 0xffec407a.toInt())
    override fun onDraw(canvas: Canvas) {
        canvas.drawColor(0xff101724.toInt())
        val unit = min(width / 800f, height / 480f)
        if (unit <= 0f) return
        canvas.save()
        val panelHeight = height / unit
        canvas.translate((width - 800 * unit) / 2, 0f)
        canvas.scale(unit, unit)
        text(canvas, race?.track ?: hub?.name ?: "Crash Team Racing", 24f, 40f, 26f, Color.WHITE)
        val data = race
        if(tintSource !== portraits) { glyphTints.clear(); tintSource=portraits }
        if (data == null) {
            hasDrawnArtwork = false
            if(hub!=null && drawHub(canvas,hub!!,panelHeight)) {canvas.restore();return}
            drawWelcome(canvas,panelHeight)
            canvas.restore(); return
        }
        text(canvas, "POSITIONS", 12f, 91f, 19f, Color.LTGRAY)
        val hud=data.hud
        hudText(canvas, "TIME", RectF(246f,14f,390f,36f), true, false)
        hudText(canvas, hud.clock(), RectF(246f,42f,390f,88f), false, false)
        hudText(canvas, "LAP", RectF(408f,14f,490f,36f), true, true)
        hudText(canvas, "${hud.lap.coerceAtMost(hud.totalLaps.coerceAtLeast(1))}/${hud.totalLaps}",
            RectF(408f,42f,490f,88f), false, true)
        portraits[CtrPortraitReader.WUMPA]?.let { picture(canvas,it,RectF(506f,14f,558f,72f)) }
            ?: text(canvas,"WUMPA",506f,32f,14f,Color.LTGRAY)
        hudText(canvas,hud.wumpa.toString(),RectF(560f,36f,614f,86f),false,false)
        // A proper item slot stays visible even when empty or during roulette.
        paint.color=if(hud.wumpa==10) 0xffffc857.toInt() else 0xff76869f.toInt()
        canvas.drawRoundRect(RectF(626f,6f,790f,126f),10f,10f,paint)
        paint.color=0xff182438.toInt()
        canvas.drawRoundRect(RectF(629f,9f,787f,123f),8f,8f,paint)
        val itemIcon=if(hud.item==16) {
            // Cosmetic cycling only: never changes the selected weapon or game RNG.
            val cycle=intArrayOf(5,6,7,8,9,11,12,13,14,15,16)
            cycle[((SystemClock.uptimeMillis()/100)%cycle.size).toInt()]
        } else hud.itemIcon()
        val itemPicture=portraits[100+itemIcon]
        if(itemPicture!=null) picture(canvas,itemPicture,RectF(640f,15f,775f,110f))
        else if(hud.item!=15) text(canvas,"Loading item…",641f,69f,14f,Color.LTGRAY)
        if(hud.item!=15 && hud.item!=16 && hud.itemCount>0)
            hudText(canvas,hud.itemCount.toString(),RectF(752f,90f,782f,119f),true,false)
        val points = data.edges.flatMap { listOf(it.from, it.to) }
        val minX = points.minOf { it.x }; val maxX = points.maxOf { it.x }
        val minZ = points.minOf { it.z }; val maxZ = points.maxOf { it.z }
        val mapBottom=panelHeight-48f
        val scale = min(474f / (maxX - minX).coerceAtLeast(1f),
            (mapBottom-190f).coerceAtLeast(1f) / (maxZ - minZ).coerceAtLeast(1f))
        fun x(p: CtrRaceReader.Point) = 511f + (p.x - (minX + maxX) / 2) * scale
        fun y(p: CtrRaceReader.Point) = (132f+mapBottom)/2 - (p.z - (minZ + maxZ) / 2) * scale
        // A racer outside the checkpoint bounds must not paint over the standings.
        canvas.save()
        canvas.clipRect(240f, 132f, 782f, mapBottom)
        paint.color = 0xff66778f.toInt(); paint.strokeWidth = 7f; paint.strokeCap = Paint.Cap.ROUND
        data.edges.forEach { canvas.drawLine(x(it.from), y(it.from), x(it.to), y(it.to), paint) }
        data.racers.asReversed().forEach {
            val portrait = portraits[it.character]
            paint.color = if (it.id == 0) Color.WHITE else colors[it.id]
            canvas.drawRoundRect(RectF(x(it.point)-29f, y(it.point)-29f, x(it.point)+29f, y(it.point)+29f), 5f, 5f, paint)
            paint.color = Color.BLACK
            canvas.drawRect(x(it.point)-26f, y(it.point)-26f, x(it.point)+26f, y(it.point)+26f, paint)
            if (portrait != null) picture(canvas, portrait,
                RectF(x(it.point)-25f, y(it.point)-25f, x(it.point)+25f, y(it.point)+25f))
            else text(canvas, it.position.toString(), x(it.point)-4f, y(it.point)+5f, 13f, Color.WHITE)
        }
        canvas.restore()
        val footer=panelHeight-16f
        text(canvas, "BOOST", 240f, footer, 13f, Color.LTGRAY)
        paint.color=0xff26374c.toInt();canvas.drawRect(296f,footer-13f,446f,footer,paint)
        val boost=if(hud.driftRoom==0 || hud.driftMax<=0) 0f else
            (1f-hud.driftRoom.toFloat()/hud.driftMax).coerceIn(0f,1f)
        paint.color=if(boost>0.7f) 0xffffa726.toInt() else 0xff66bb6a.toInt()
        canvas.drawRect(296f,footer-13f,296f+150f*boost,footer,paint)
        text(canvas, "×${hud.boosts.coerceAtLeast(0)}", 454f, footer, 14f, Color.WHITE)
        text(canvas, if(hud.wrongWay) "WRONG WAY!" else "Jump ${hud.jump.coerceAtLeast(0)}", 536f, footer, 14f,
            if(hud.wrongWay) 0xffffa726.toInt() else Color.LTGRAY)
        val slots = data.racers.associateBy { it.position }
        val rowHeight=(panelHeight-120f)/8
        for (position in 1..8) {
            val racer = slots[position]
            val top = 104f + (position-1)*rowHeight
            val bottom=top+rowHeight-4f
            paint.color = if (racer?.id == 0) Color.WHITE else 0xff26374c.toInt()
            canvas.drawRoundRect(RectF(8f, top, 224f, bottom), 6f, 6f, paint)
            paint.color = 0xff101724.toInt()
            canvas.drawRoundRect(RectF(11f, top+3f, 221f, bottom-3f), 4f, 4f, paint)
            val portrait = racer?.let { portraits[it.character] }
            val portraitSize=(rowHeight-12f).coerceAtMost(80f)
            val portraitTop=(top+bottom-portraitSize)/2
            if (portrait != null) picture(canvas, portrait, RectF(52f, portraitTop, 52f+portraitSize, portraitTop+portraitSize))
            else centered(canvas, "—", 86f, (top+bottom)/2+6f, 18f, Color.LTGRAY)
            centered(canvas, position.toString(), 29f, (top+bottom)/2+11f, 32f,
                if (racer?.id == 0) Color.WHITE else 0xffffc857.toInt())
            // Names are a useful fallback if a portrait has not loaded. Fit them to the column.
            paint.textSize=15f
            val name=racer?.name ?: "—"
            val nameSize=if(paint.measureText(name)>78f) 15f*78f/paint.measureText(name) else 15f
            text(canvas, name, 136f, (top+bottom)/2-3f, nameSize.coerceAtLeast(10f), Color.LTGRAY)
            text(canvas, if(racer==null) "" else "Lap ${racer.lap}", 136f, (top+bottom)/2+17f, 13f, Color.LTGRAY)
        }
        hasDrawnArtwork=hudArtworkReady(portraits) && data.racers.all { portraits[it.character]!=null } &&
            (hud.item==15 || itemPicture!=null)
        canvas.restore()
    }
    private fun drawWelcome(canvas:Canvas,panelHeight:Float) {
        paint.shader=LinearGradient(0f,64f,800f,panelHeight,0xff172b46.toInt(),0xff101724.toInt(),Shader.TileMode.CLAMP)
        canvas.drawRect(0f,64f,800f,panelHeight,paint);paint.shader=null
        val center=panelHeight*0.38f
        // Code-native checker motif; no downloaded/bundled game artwork.
        for(row in 0..1)for(col in 0..15) {
            paint.color=if((row+col)%2==0) 0xffe2e7f0.toInt() else 0xff24374e.toInt()
            canvas.drawRect(240f+col*20f,center-120f+row*16f,260f+col*20f,center-104f+row*16f,paint)
        }
        paint.typeface=Typeface.create(Typeface.SANS_SERIF,Typeface.BOLD_ITALIC)
        centered(canvas,"CTR",400f,center-42f,68f,0xffffc857.toInt())
        paint.typeface=Typeface.DEFAULT
        centered(canvas,"SECOND-SCREEN COMPANION",400f,center-12f,18f,Color.LTGRAY)
        paint.color=0xff263e59.toInt()
        canvas.drawRoundRect(RectF(40f,center+12f,760f,center+130f),16f,16f,paint)
        centered(canvas,screen.title,400f,center+50f,28f,Color.WHITE)
        val description=if(hub!=null) "Preparing your Adventure map…" else screen.subtitle
        val lines=wrap(description,650f,18f)
        lines.take(2).forEachIndexed { index,line -> centered(canvas,line,400f,center+82f+index*24f,18f,Color.LTGRAY) }
        if(screen.kind==CtrScreenState.Kind.RESULTS && lastRace!=null) {
            val previous=lastRace!!
            centered(canvas,"${previous.track} · ${previous.hud.clock()}",400f,center+166f,22f,0xffffc857.toInt())
        }
        centered(canvas,"Live map • Eight racer positions • Items, laps and boost",400f,panelHeight-78f,18f,0xffbdcde2.toInt())
        centered(canvas,"Use the top-screen controls. Race view switches automatically.",400f,panelHeight-44f,17f,Color.LTGRAY)
    }
    private fun drawHub(canvas:Canvas,data:CtrHubReader.Hub,panelHeight:Float):Boolean {
        val bottom=portraits[CtrPortraitReader.HUB_BOTTOM] ?: return false
        val top=if(data.showTop) portraits[CtrPortraitReader.HUB_TOP] ?: return false else null
        text(canvas,"ADVENTURE MAP",24f,83f,21f,0xffffc857.toInt())
        text(canvas,"Explore the hub before your next race",24f,108f,16f,Color.LTGRAY)
        val bounds=RectF(34f,128f,766f,panelHeight-64f)
        paint.color=0xff182438.toInt();canvas.drawRoundRect(bounds,14f,14f,paint)
        val mapScale=min((bounds.width()-40f)/data.width,(bounds.height()-40f)/data.height)
        val left=bounds.centerX()-data.width*mapScale/2
        val upper=bounds.centerY()-data.height*mapScale/2
        // Both halves are right-aligned just like UI_Map_DrawMap; top may be absent.
        if(top!=null)picture(canvas,top,RectF(left+(data.width-data.topWidth)*mapScale,upper,
            left+data.width*mapScale,upper+data.topHeight*mapScale))
        val bottomY=upper+(if(data.showTop)data.topHeight else 0)*mapScale
        picture(canvas,bottom,RectF(left+(data.width-data.bottomWidth)*mapScale,bottomY,
            left+data.width*mapScale,bottomY+data.bottomHeight*mapScale))
        val x=left+data.x.coerceIn(0f,1f)*data.width*mapScale
        val y=upper+data.y.coerceIn(0f,1f)*data.height*mapScale
        canvas.save();canvas.clipRect(bounds)
        paint.color=Color.WHITE;canvas.drawRoundRect(RectF(x-31f,y-31f,x+31f,y+31f),8f,8f,paint)
        paint.color=0xff101724.toInt();canvas.drawRect(x-28f,y-28f,x+28f,y+28f,paint)
        portraits[data.character]?.let {picture(canvas,it,RectF(x-26f,y-26f,x+26f,y+26f))}
            ?: centered(canvas,"YOU",x,y+6f,17f,0xffffc857.toInt())
        canvas.restore()
        centered(canvas,"Your position updates as you drive • Race HUD appears inside races",400f,panelHeight-27f,17f,Color.LTGRAY)
        return true
    }
    private fun wrap(value:String,width:Float,size:Float):List<String> {
        paint.textSize=size;val lines=ArrayList<String>();var line=""
        value.split(' ').forEach { word ->
            val next=if(line.isEmpty())word else "$line $word"
            if(paint.measureText(next)>width && line.isNotEmpty()){lines.add(line);line=word}else line=next
        }
        if(line.isNotEmpty())lines.add(line);return lines
    }
    private fun hudText(canvas: Canvas,value:String,bounds:RectF,small:Boolean,purple:Boolean) {
        val glyphs=value.map { if(it==' ') null else portraits[CtrPortraitReader.glyphKey(it,small)] }
        if(value.indices.any { value[it]!=' ' && glyphs[it]==null }) {
            text(canvas,value,bounds.left,bounds.bottom, min(18f,bounds.height()),Color.WHITE);return
        }
        val glyphHeight=glyphs.filterNotNull().maxOfOrNull { it.height }?.toFloat() ?: return
        val glyphWidth=glyphs.sumOf { (it?.width ?: (glyphHeight/2).toInt())+1 }.toFloat()
        val scale=min(bounds.height()/glyphHeight,bounds.width()/glyphWidth)
        var x=bounds.left
        paint.isFilterBitmap=false
        value.indices.forEach { index ->
            val glyph=glyphs[index]
            if(glyph!=null) {
                // Texture glyph shapes are original; match the HUD's orange/purple gradient.
                val y=bounds.centerY()-glyph.height*scale/2
                val tinted=glyphTints.getOrPut(glyph to purple) {
                    val pixels=IntArray(glyph.width*glyph.height)
                    glyph.getPixels(pixels,0,glyph.width,0,0,glyph.width,glyph.height)
                    val top=if(purple) 0xffdedcff.toInt() else 0xffffe65b.toInt()
                    val bottom=if(purple) 0xff807bd9.toInt() else 0xffed711d.toInt()
                    pixels.indices.forEach { pixel ->
                        val t=(pixel/glyph.width).toFloat()/glyph.height.coerceAtLeast(1)
                        val original=pixels[pixel]
                        fun channel(shift:Int):Int {
                            val gradient=(((top shr shift) and 255)*(1-t)+((bottom shr shift) and 255)*t)
                            return (((original shr shift) and 255)*gradient/128f).toInt().coerceAtMost(255)
                        }
                        pixels[pixel]=(original and 0xff000000.toInt()) or (channel(16) shl 16) or
                            (channel(8) shl 8) or channel(0)
                    }
                    Bitmap.createBitmap(pixels,glyph.width,glyph.height,Bitmap.Config.ARGB_8888)
                }
                paint.color=Color.WHITE
                canvas.drawBitmap(tinted,null,RectF(x,y,x+glyph.width*scale,y+glyph.height*scale),paint)
            }
            x+=((glyph?.width ?: (glyphHeight/2).toInt())+1)*scale
        }
    }
    private fun picture(canvas: Canvas, bitmap: Bitmap, bounds: RectF) {
        val scale = min(bounds.width()/bitmap.width, bounds.height()/bitmap.height)
        val w=bitmap.width*scale; val h=bitmap.height*scale
        paint.color = Color.WHITE
        paint.isFilterBitmap = false // Preserve the original pixel-art portraits.
        canvas.drawBitmap(bitmap, null, RectF(bounds.centerX()-w/2, bounds.centerY()-h/2,
            bounds.centerX()+w/2, bounds.centerY()+h/2), paint)
    }
    private fun centered(canvas: Canvas, value: String, x: Float, y: Float, size: Float, color: Int) {
        paint.textSize=size
        text(canvas, value, x-paint.measureText(value)/2, y, size, color)
    }
    private fun text(canvas: Canvas, value: String, x: Float, y: Float, size: Float, color: Int) {
        paint.color = color; paint.textSize = size; paint.style = Paint.Style.FILL
        canvas.drawText(value, x, y, paint)
    }
}
