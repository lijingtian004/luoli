package com.luoli.modifier.service

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.graphics.PixelFormat
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.os.IBinder
import android.text.Editable
import android.text.TextWatcher
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.HorizontalScrollView
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.ProgressBar
import android.widget.Spinner
import android.widget.TextView
import com.luoli.modifier.core.EngineProtocol
import com.luoli.modifier.core.EngineRepo
import com.luoli.modifier.core.HitUi
import com.luoli.modifier.overlay.AiActivityBus
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlin.math.abs

/**
 * 悬浮窗服务：悬浮球 → 面板（手动模式 / AI 模式）
 * 手动模式 = 完整修改器操作台（与主界面共享 EngineRepo 状态）
 * AI 模式 = 实时显示 AI agent 的 MCP 工具调用活动
 */
class OverlayService : Service() {

    companion object {
        const val CHANNEL_ID = "overlay"
        const val NOTIFY_ID = 2
        const val MODE_MANUAL = 0
        const val MODE_AI = 1

        val TYPES = listOf("i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32", "f64")
        val OPS = listOf(
            "eq" to "=", "ne" to "≠", "gt" to ">", "ge" to "≥", "lt" to "<", "le" to "≤",
            "inc" to "增", "dec" to "减", "changed" to "变", "unchanged" to "未变",
        )
        private val ACCENT = 0xFFFF7BA9.toInt()
        private val BG = 0xF2121218.toInt()

        fun start(ctx: Context) {
            ctx.startForegroundService(Intent(ctx, OverlayService::class.java))
        }

        fun stop(ctx: Context) {
            ctx.stopService(Intent(ctx, OverlayService::class.java))
        }
    }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private lateinit var wm: WindowManager
    private var ballView: View? = null
    private var panelView: View? = null

    // 手动面板控件
    private lateinit var statusText: TextView
    private lateinit var aiStatusText: TextView
    private lateinit var typeSpinner: Spinner
    private lateinit var valueEdit: EditText
    private lateinit var progress: ProgressBar
    private lateinit var resultListView: ListView
    private val resultItems = mutableListOf<String>()
    private lateinit var resultAdapter: ArrayAdapter<String>
    private var currentHits: List<HitUi> = emptyList()
    private lateinit var addrEdit: EditText
    private lateinit var editValue: EditText
    private val opButtons = mutableMapOf<String, Button>()
    private var selectedOp = "eq"

    // AI 面板
    private lateinit var aiListView: ListView
    private var chipManual: TextView? = null
    private var chipAi: TextView? = null

    private val aiItems = mutableListOf<String>()
    private lateinit var aiAdapter: ArrayAdapter<String>

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        wm = getSystemService(Context.WINDOW_SERVICE) as WindowManager
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        nm.createNotificationChannel(
            NotificationChannel(CHANNEL_ID, "悬浮窗", NotificationManager.IMPORTANCE_MIN)
        )
        val n: Notification = Notification.Builder(this, CHANNEL_ID)
            .setContentTitle("悬浮窗运行中")
            .setSmallIcon(android.R.drawable.ic_menu_view)
            .setOngoing(true)
            .build()
        startForeground(NOTIFY_ID, n)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        showBall()
        return START_STICKY
    }

    override fun onDestroy() {
        scope.cancel()
        removeViews()
        super.onDestroy()
    }

    private fun removeViews() {
        ballView?.let { runCatching { wm.removeView(it) } }
        panelView?.let { runCatching { wm.removeView(it) } }
        ballView = null
        panelView = null
    }

    private fun dp(v: Int): Int = (v * resources.displayMetrics.density).toInt()

    // ---------- 悬浮球 ----------

    @SuppressLint("ClickableViewAccessibility")
    private fun showBall() {
        panelView?.let { wm.removeView(it); panelView = null }
        if (ballView != null) return

        val frame = FrameLayout(this)
        val ball = TextView(this).apply {
            text = "萝"
            textSize = 20f
            setTextColor(Color.WHITE)
            gravity = Gravity.CENTER
            typeface = Typeface.DEFAULT_BOLD
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL
                setColor(0xE0C2185B.toInt())
            }
        }
        frame.addView(ball, FrameLayout.LayoutParams(dp(46), dp(46)))

        val lp = WindowManager.LayoutParams(
            dp(46), dp(46),
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE
                or WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.START
            x = dp(8); y = dp(140)
        }

        frame.setOnClickListener { showPanel() }
        makeDraggable(frame, lp)
        wm.addView(frame, lp)
        ballView = frame
    }

    private fun showPanel() {
        ballView?.let { wm.removeView(it); ballView = null }
        if (panelView != null) return
        buildPanel()
        val lp = WindowManager.LayoutParams(
            (resources.displayMetrics.widthPixels * 0.88f).toInt().coerceAtMost(dp(340)),
            dp(440),
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.START
            x = dp(12); y = dp(50)
        }
        makeDraggable(panelView!!, lp)
        wm.addView(panelView!!, lp)
        registerFlowCollectors()
    }

    @SuppressLint("ClickableViewAccessibility")
    private fun makeDraggable(v: View, lp: WindowManager.LayoutParams) {
        var downX = 0f; var downY = 0f; var startX = 0; var startY = 0; var dragging = false
        v.setOnTouchListener { view, ev ->
            val params = view.layoutParams as WindowManager.LayoutParams
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    downX = ev.rawX; downY = ev.rawY
                    startX = params.x; startY = params.y
                    dragging = false
                    false
                }
                MotionEvent.ACTION_MOVE -> {
                    val dx = ev.rawX - downX; val dy = ev.rawY - downY
                    if (dragging || abs(dx) > dp(6) || abs(dy) > dp(6)) {
                        dragging = true
                        params.x = startX + dx.toInt()
                        params.y = startY + dy.toInt()
                        runCatching { wm.updateViewLayout(view, params) }
                    }
                    dragging
                }
                MotionEvent.ACTION_UP -> dragging
                else -> false
            }
        }
    }

    // ---------- 面板构建 ----------

    private fun buildPanel() {
        val ctx = this
        val root = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            background = GradientDrawable().apply {
                setColor(BG)
                cornerRadius = dp(12).toFloat()
            }
            setPadding(dp(10), dp(8), dp(10), dp(10))
        }
        panelView = root

        // 头部
        val header = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        header.addView(TextView(ctx).apply {
            text = "萝莉修改器"
            textSize = 13f
            setTextColor(Color.WHITE)
            typeface = Typeface.DEFAULT_BOLD
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        })
        header.addView(chip(ctx, "手动") { switchContent(MODE_MANUAL, null) }.also { chipManual = it })
        header.addView(chip(ctx, "AI") { switchContent(MODE_AI, null) }.also { chipAi = it })
        header.addView(chip(ctx, "—") { showBall() })
        root.addView(header)

        statusText = TextView(ctx).apply {
            textSize = 11f
            setTextColor(0xFF9E9E9E.toInt())
            setPadding(0, dp(4), 0, 0)
        }
        root.addView(statusText)

        // 手动内容
        root.addView(buildManualContent())
        // AI 内容
        root.addView(buildAiContent())
        switchContent(MODE_MANUAL, null)
    }

    private fun chip(ctx: Context, text: String, onClick: (View) -> Unit) = TextView(ctx).apply {
        this.text = text
        textSize = 12f
        setTextColor(ACCENT)
        setPadding(dp(8), dp(4), dp(8), dp(4))
        setOnClickListener(onClick)
    }

    private lateinit var manualBox: LinearLayout
    private lateinit var aiBox: LinearLayout

    private fun switchContent(mode: Int, view: View?) {
        manualBox.visibility = if (mode == MODE_MANUAL) View.VISIBLE else View.GONE
        aiBox.visibility = if (mode == MODE_AI) View.VISIBLE else View.GONE
        chipManual?.setTextColor(if (mode == MODE_MANUAL) ACCENT else 0xFF888888.toInt())
        chipAi?.setTextColor(if (mode == MODE_AI) ACCENT else 0xFF888888.toInt())
    }

    private fun buildManualContent(): View {
        val ctx = this
        manualBox = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }

        // 第一行：类型 + 数值 + 搜索
        val row1 = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        typeSpinner = Spinner(ctx).apply {
            adapter = ArrayAdapter(ctx, android.R.layout.simple_spinner_dropdown_item, TYPES)
            setSelection(4)
            layoutParams = LinearLayout.LayoutParams(dp(88), ViewGroup.LayoutParams.WRAP_CONTENT)
        }
        row1.addView(typeSpinner)
        valueEdit = EditText(ctx).apply {
            hint = "数值"
            textSize = 12f
            setSingleLine(true)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        row1.addView(valueEdit)
        row1.addView(button(ctx, "搜索") {
            EngineRepo.launch {
                EngineRepo.search(
                    TYPES.getOrNull(typeSpinner.selectedItemPosition) ?: "i32",
                    valueEdit.text.toString(), 4, ""
                )
            }
        })
        manualBox.addView(row1)

        // 过滤操作行
        val opsScroll = HorizontalScrollView(ctx).apply { isHorizontalScrollBarEnabled = false }
        val opsRow = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        OPS.forEach { (op, label) ->
            opsRow.addView(Button(ctx).apply {
                text = label
                textSize = 11f
                setTextColor(Color.WHITE)
                minWidth = 0
                minimumWidth = 0
                setPadding(dp(10), 0, dp(10), 0)
                isAllCaps = false
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, dp(34)
                ).apply { setMargins(0, 0, dp(4), 0) }
                setOnClickListener {
                    selectedOp = op
                    updateOpColors()
                }
                opButtons[op] = this
            })
        }
        opsScroll.addView(opsRow)
        manualBox.addView(opsScroll)

        // 第二行：过滤 + 清空 + 刷新 + 进度
        val row2 = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        row2.addView(button(ctx, "过滤") {
            EngineRepo.launch {
                val needsValue = selectedOp !in setOf("inc", "dec", "changed", "unchanged")
                EngineRepo.filter(
                    selectedOp,
                    TYPES.getOrNull(typeSpinner.selectedItemPosition) ?: "i32",
                    if (needsValue) valueEdit.text.toString() else null
                )
            }
        })
        row2.addView(button(ctx, "清空") { EngineRepo.launch { EngineRepo.reset() } })
        row2.addView(button(ctx, "刷新") { EngineRepo.launch { EngineRepo.refreshResults() } })
        manualBox.addView(row2)

        progress = ProgressBar(ctx, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 1000
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, dp(10)
            ).apply { setMargins(0, dp(2), 0, dp(2)) }
        }
        manualBox.addView(progress)

        // 结果列表
        resultAdapter = ArrayAdapter(ctx, android.R.layout.simple_list_item_1, resultItems)
        resultListView = ListView(ctx).apply {
            adapter = resultAdapter
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f
            ).apply { setMargins(0, dp(4), 0, 0) }
            setOnItemClickListener { _, _, pos, _ ->
                currentHits.getOrNull(pos)?.let { h ->
                    addrEdit.setText(EngineProtocol.longToHex(h.addr))
                    editValue.setText(
                        if (h.type in setOf("f32", "f64")) h.value.toString() else h.value.toLong().toString()
                    )
                }
            }
        }
        manualBox.addView(resultListView)

        // 底部编辑行
        val row3 = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        addrEdit = EditText(ctx).apply {
            hint = "地址"
            textSize = 11f
            setSingleLine(true)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.2f)
        }
        row3.addView(addrEdit)
        editValue = EditText(ctx).apply {
            hint = "新值"
            textSize = 11f
            setSingleLine(true)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        row3.addView(editValue)
        row3.addView(button(ctx, "写入") {
            doWrite(freeze = false)
        })
        row3.addView(button(ctx, "冻结") {
            doWrite(freeze = true)
        })
        manualBox.addView(row3)
        return manualBox
    }

    private fun doWrite(freeze: Boolean) {
        val addrText = addrEdit.text.toString().trim()
        val v = editValue.text.toString().trim()
        if (addrText.isEmpty() || v.isEmpty()) return
        val type = TYPES.getOrNull(typeSpinner.selectedItemPosition) ?: "i32"
        val addr = try { EngineProtocol.hexToLong(addrText) } catch (t: Throwable) { return }
        EngineRepo.launch {
            if (freeze) EngineRepo.freeze(addr, type, v, 150)
            else EngineRepo.write(addr, type, v)
        }
    }

    private fun buildAiContent(): View {
        val ctx = this
        aiBox = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
        }
        aiStatusText = TextView(ctx).apply {
            textSize = 11f
            setTextColor(ACCENT)
            setPadding(0, dp(2), 0, dp(2))
        }
        aiBox.addView(aiStatusText)
        aiAdapter = ArrayAdapter(ctx, android.R.layout.simple_list_item_1, aiItems)
        aiListView = ListView(ctx).apply {
            adapter = aiAdapter
            layoutParams = LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f
            )
        }
        aiBox.addView(aiListView)
        return aiBox
    }

    private fun updateOpColors() {
        OPS.forEach { (op, _) ->
            opButtons[op]?.setTextColor(if (op == selectedOp) ACCENT else Color.WHITE)
        }
    }

    private fun button(ctx: Context, text: String, onClick: () -> Unit) = Button(ctx).apply {
        this.text = text
        textSize = 11f
        isAllCaps = false
        minWidth = 0
        minimumWidth = 0
        setPadding(dp(10), 0, dp(10), 0)
        setTextColor(Color.WHITE)
        layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, dp(36)
        ).apply { setMargins(dp(3), 0, 0, 0) }
        setOnClickListener { onClick() }
    }

    // ---------- 数据绑定 ----------

    private var collectorsStarted = false

    private fun registerFlowCollectors() {
        if (collectorsStarted) return
        collectorsStarted = true

        scope.launch {
            EngineRepo.attached.collect { a ->
                statusText.text = if (a != null) "目标: ${a.name} (pid=${a.pid})"
                else "未附加（到主界面进程页附加）"
            }
        }
        scope.launch {
            EngineRepo.hits.collect { hits ->
                currentHits = hits
                resultItems.clear()
                resultItems.addAll(hits.map { h ->
                    val v = if (h.type in setOf("f32", "f64")) "%.4f".format(h.value)
                    else h.value.toLong().toString()
                    "0x%016X  $v  (${h.type})".format(h.addr)
                })
                resultAdapter.notifyDataSetChanged()
            }
        }
        scope.launch {
            EngineRepo.progress.collect { p -> progress.progress = (p * 1000).toInt() }
        }
        scope.launch {
            EngineRepo.total.collect { t ->
                aiStatusText.text = if (EngineRepo.scanning.value) "AI 操作中…" else "结果: $t"
            }
        }
        scope.launch {
            AiActivityBus.lines.collect { lines ->
                aiItems.clear()
                aiItems.addAll(lines)
                aiAdapter.notifyDataSetChanged()
                aiListView.setSelection(lines.size.coerceAtLeast(1) - 1)
            }
        }
    }
}
