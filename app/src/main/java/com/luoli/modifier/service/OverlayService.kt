package com.luoli.modifier.service

import android.annotation.SuppressLint
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.ClipData
import android.content.ClipboardManager
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
import android.widget.ArrayAdapter
import android.widget.BaseAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.ProgressBar
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import com.luoli.modifier.core.EngineProtocol
import com.luoli.modifier.core.EngineRepo
import com.luoli.modifier.core.FrozenUi
import com.luoli.modifier.core.HitUi
import com.luoli.modifier.core.ProcessUi
import com.luoli.modifier.overlay.AiActivityBus
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlin.math.abs

/**
 * 悬浮窗服务：悬浮球 ↔ 浮窗面板
 * 包含：进程切换附加（前台优先）、内存搜索过滤与全改、数值冻结监控管理、AI 智能体操作流
 */
class OverlayService : Service() {

    companion object {
        const val CHANNEL_ID = "overlay"
        const val NOTIFY_ID = 2

        const val TAB_SEARCH = 0
        const val TAB_PROCESS = 1
        const val TAB_FROZEN = 2
        const val TAB_AI = 3

        val TYPES = listOf("i8", "u8", "i16", "u16", "i32", "u32", "i64", "u64", "f32", "f64")
        val OPS = listOf(
            "eq" to "=", "ne" to "≠", "gt" to ">", "ge" to "≥", "lt" to "<", "le" to "≤",
            "inc" to "增", "dec" to "减", "changed" to "变", "unchanged" to "未变",
        )
        private val ACCENT = 0xFFFF7BA9.toInt()
        private val BG = 0xF2121218.toInt()
        private val CARD_BG = 0x22FFFFFF.toInt()

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
    private var panelParams: WindowManager.LayoutParams? = null

    private var currentTab = TAB_SEARCH
    private var isTransparent = false

    // 顶部公共控件
    private lateinit var statusText: TextView
    private var chipProcess: TextView? = null
    private var chipSearch: TextView? = null
    private var chipFrozen: TextView? = null
    private var chipAi: TextView? = null
    private var chipOpacity: TextView? = null

    // 容器
    private lateinit var processBox: LinearLayout
    private lateinit var searchBox: LinearLayout
    private lateinit var frozenBox: LinearLayout
    private lateinit var aiBox: LinearLayout

    // 进程页控件
    private lateinit var procSearchEdit: EditText
    private lateinit var procListView: ListView
    private var procList = listOf<ProcessUi>()
    private var procIncludeSystem = false
    private lateinit var procSysBtn: Button

    // 搜索页控件
    private lateinit var typeSpinner: Spinner
    private lateinit var valueEdit: EditText
    private lateinit var progress: ProgressBar
    private lateinit var searchCountText: TextView
    private lateinit var resultListView: ListView
    private val resultItems = mutableListOf<String>()
    private lateinit var resultAdapter: ArrayAdapter<String>
    private var currentHits: List<HitUi> = emptyList()
    private lateinit var addrEdit: EditText
    private lateinit var editValue: EditText
    private val opButtons = mutableMapOf<String, Button>()
    private var selectedOp = "eq"

    // 冻结页控件
    private lateinit var frozenCountText: TextView
    private lateinit var frozenListView: ListView
    private var frozenItems = listOf<FrozenUi>()

    // AI 页控件
    private lateinit var aiStatusText: TextView
    private lateinit var aiListView: ListView
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
            (resources.displayMetrics.widthPixels * 0.90f).toInt().coerceAtMost(dp(360)),
            dp(480),
            WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL
                or WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN,
            PixelFormat.TRANSLUCENT
        ).apply {
            gravity = Gravity.TOP or Gravity.START
            x = dp(12); y = dp(50)
            softInputMode = WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE
            alpha = if (isTransparent) 0.65f else 0.95f
        }
        panelParams = lp
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

        // 头部导航栏
        val header = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        header.addView(TextView(ctx).apply {
            text = "萝莉"
            textSize = 14f
            setTextColor(Color.WHITE)
            typeface = Typeface.DEFAULT_BOLD
            setPadding(0, 0, dp(6), 0)
        })

        header.addView(tabChip(ctx, "搜索") { switchTab(TAB_SEARCH) }.also { chipSearch = it })
        header.addView(tabChip(ctx, "进程") { switchTab(TAB_PROCESS) }.also { chipProcess = it })
        header.addView(tabChip(ctx, "冻结") { switchTab(TAB_FROZEN) }.also { chipFrozen = it })
        header.addView(tabChip(ctx, "AI") { switchTab(TAB_AI) }.also { chipAi = it })

        // 占位弹簧
        val spacer = View(ctx).apply {
            layoutParams = LinearLayout.LayoutParams(0, 1, 1f)
        }
        header.addView(spacer)

        header.addView(actionChip(ctx, if (isTransparent) "实色" else "半透") { toggleOpacity() }.also { chipOpacity = it })
        header.addView(actionChip(ctx, "—") { showBall() })
        header.addView(actionChip(ctx, "✕") { stopSelf() })
        root.addView(header)

        // 目标状态指示条（点击可直接跳到进程列表页）
        statusText = TextView(ctx).apply {
            textSize = 11f
            setTextColor(0xFFB0B0B0.toInt())
            setPadding(dp(2), dp(4), dp(2), dp(4))
            setOnClickListener { switchTab(TAB_PROCESS) }
        }
        root.addView(statusText)

        // 四大模块容器
        root.addView(buildSearchContent())
        root.addView(buildProcessContent())
        root.addView(buildFrozenContent())
        root.addView(buildAiContent())

        switchTab(TAB_SEARCH)
    }

    private fun tabChip(ctx: Context, text: String, onClick: (View) -> Unit) = TextView(ctx).apply {
        this.text = text
        textSize = 12f
        setTextColor(Color.GRAY)
        setPadding(dp(6), dp(4), dp(6), dp(4))
        setOnClickListener(onClick)
    }

    private fun actionChip(ctx: Context, text: String, onClick: (View) -> Unit) = TextView(ctx).apply {
        this.text = text
        textSize = 12f
        setTextColor(Color.WHITE)
        setPadding(dp(6), dp(4), dp(6), dp(4))
        setOnClickListener(onClick)
    }

    private fun toggleOpacity() {
        isTransparent = !isTransparent
        panelParams?.alpha = if (isTransparent) 0.65f else 0.95f
        panelView?.let { runCatching { wm.updateViewLayout(it, panelParams) } }
        chipOpacity?.text = if (isTransparent) "实色" else "半透"
    }

    private fun switchTab(tab: Int) {
        currentTab = tab
        searchBox.visibility = if (tab == TAB_SEARCH) View.VISIBLE else View.GONE
        processBox.visibility = if (tab == TAB_PROCESS) View.VISIBLE else View.GONE
        frozenBox.visibility = if (tab == TAB_FROZEN) View.VISIBLE else View.GONE
        aiBox.visibility = if (tab == TAB_AI) View.VISIBLE else View.GONE

        chipSearch?.setTextColor(if (tab == TAB_SEARCH) ACCENT else 0xFF888888.toInt())
        chipProcess?.setTextColor(if (tab == TAB_PROCESS) ACCENT else 0xFF888888.toInt())
        chipFrozen?.setTextColor(if (tab == TAB_FROZEN) ACCENT else 0xFF888888.toInt())
        chipAi?.setTextColor(if (tab == TAB_AI) ACCENT else 0xFF888888.toInt())

        if (tab == TAB_PROCESS) refreshProcesses()
        if (tab == TAB_FROZEN) refreshFrozenList()
    }

    // ---------- 搜索页 ----------

    private fun buildSearchContent(): View {
        val ctx = this
        searchBox = LinearLayout(ctx).apply { orientation = LinearLayout.VERTICAL }

        // 第一行：类型 + 数值 + 搜索
        val row1 = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        typeSpinner = Spinner(ctx).apply {
            adapter = ArrayAdapter(ctx, android.R.layout.simple_spinner_dropdown_item, TYPES)
            setSelection(4)
            layoutParams = LinearLayout.LayoutParams(dp(84), ViewGroup.LayoutParams.WRAP_CONTENT)
        }
        row1.addView(typeSpinner)

        valueEdit = EditText(ctx).apply {
            hint = "数值"
            textSize = 12f
            setTextColor(Color.WHITE)
            setHintTextColor(0xFF888888.toInt())
            setSingleLine(true)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        row1.addView(valueEdit)

        row1.addView(button(ctx, "搜索") {
            val v = valueEdit.text.toString().trim()
            if (v.isEmpty()) {
                Toast.makeText(ctx, "请输入搜索数值", Toast.LENGTH_SHORT).show()
                return@button
            }
            EngineRepo.launch {
                EngineRepo.search(
                    TYPES.getOrNull(typeSpinner.selectedItemPosition) ?: "i32",
                    v, 4, ""
                )
            }
        })
        searchBox.addView(row1)

        // 过滤操作条
        val opsScroll = HorizontalScrollView(ctx).apply { isHorizontalScrollBarEnabled = false }
        val opsRow = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        OPS.forEach { (op, label) ->
            opsRow.addView(Button(ctx).apply {
                text = label
                textSize = 11f
                setTextColor(Color.WHITE)
                minWidth = 0
                minimumWidth = 0
                setPadding(dp(9), 0, dp(9), 0)
                isAllCaps = false
                layoutParams = LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, dp(32)
                ).apply { setMargins(0, 0, dp(4), 0) }
                setOnClickListener {
                    selectedOp = op
                    updateOpColors()
                }
                opButtons[op] = this
            })
        }
        opsScroll.addView(opsRow)
        searchBox.addView(opsScroll)

        // 过滤控制行：过滤 + 清空 + 刷新 + 全改
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
        row2.addView(button(ctx, "全改") {
            val targetVal = editValue.text.toString().trim().ifEmpty { valueEdit.text.toString().trim() }
            if (targetVal.isEmpty()) {
                Toast.makeText(ctx, "请输入要修改的新值", Toast.LENGTH_SHORT).show()
                return@button
            }
            val type = TYPES.getOrNull(typeSpinner.selectedItemPosition) ?: "i32"
            EngineRepo.launch {
                EngineRepo.batchWrite(type, targetVal)
            }
        })
        row2.addView(button(ctx, "清空") { EngineRepo.launch { EngineRepo.reset() } })
        row2.addView(button(ctx, "刷新") { EngineRepo.launch { EngineRepo.refreshResults() } })
        searchBox.addView(row2)

        // 进度与结果计数
        val progRow = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(2), 0, dp(2))
        }
        progress = ProgressBar(ctx, null, android.R.attr.progressBarStyleHorizontal).apply {
            max = 1000
            layoutParams = LinearLayout.LayoutParams(0, dp(8), 1f)
        }
        progRow.addView(progress)
        searchCountText = TextView(ctx).apply {
            text = "0 条"
            textSize = 11f
            setTextColor(0xFFBBBBBB.toInt())
            setPadding(dp(6), 0, 0, 0)
        }
        progRow.addView(searchCountText)
        searchBox.addView(progRow)

        // 结果列表
        resultAdapter = ArrayAdapter(ctx, android.R.layout.simple_list_item_1, resultItems)
        resultListView = ListView(ctx).apply {
            adapter = resultAdapter
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
            setOnItemClickListener { _, _, pos, _ ->
                currentHits.getOrNull(pos)?.let { h ->
                    addrEdit.setText(EngineProtocol.longToHex(h.addr))
                    editValue.setText(
                        if (h.type in setOf("f32", "f64")) h.value.toString() else h.value.toLong().toString()
                    )
                }
            }
            setOnItemLongClickListener { _, _, pos, _ ->
                currentHits.getOrNull(pos)?.let { h ->
                    val cm = getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
                    cm.setPrimaryClip(ClipData.newPlainText("addr", EngineProtocol.longToHex(h.addr)))
                    Toast.makeText(ctx, "已复制地址", Toast.LENGTH_SHORT).show()
                }
                true
            }
        }
        searchBox.addView(resultListView)

        // 底部编辑栏：地址 + 新值 + 写入 + 冻结
        val row3 = LinearLayout(ctx).apply { orientation = LinearLayout.HORIZONTAL }
        addrEdit = EditText(ctx).apply {
            hint = "地址"
            textSize = 11f
            setTextColor(Color.WHITE)
            setHintTextColor(0xFF888888.toInt())
            setSingleLine(true)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1.2f)
        }
        row3.addView(addrEdit)
        editValue = EditText(ctx).apply {
            hint = "新值"
            textSize = 11f
            setTextColor(Color.WHITE)
            setHintTextColor(0xFF888888.toInt())
            setSingleLine(true)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        row3.addView(editValue)
        row3.addView(button(ctx, "写入") { doWrite(freeze = false) })
        row3.addView(button(ctx, "冻结") { doWrite(freeze = true) })
        searchBox.addView(row3)

        return searchBox
    }

    private fun doWrite(freeze: Boolean) {
        val addrText = addrEdit.text.toString().trim()
        val v = editValue.text.toString().trim()
        if (addrText.isEmpty() || v.isEmpty()) {
            Toast.makeText(this, "地址和新值均不能为空", Toast.LENGTH_SHORT).show()
            return
        }
        val type = TYPES.getOrNull(typeSpinner.selectedItemPosition) ?: "i32"
        val addr = try { EngineProtocol.hexToLong(addrText) } catch (t: Throwable) {
            Toast.makeText(this, "地址格式错误", Toast.LENGTH_SHORT).show()
            return
        }
        EngineRepo.launch {
            if (freeze) EngineRepo.freeze(addr, type, v, 150)
            else EngineRepo.write(addr, type, v)
        }
    }

    // ---------- 进程页 ----------

    private fun buildProcessContent(): View {
        val ctx = this
        processBox = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
        }

        val topRow = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        procSearchEdit = EditText(ctx).apply {
            hint = "搜索应用名/包名…"
            textSize = 11f
            setTextColor(Color.WHITE)
            setHintTextColor(0xFF888888.toInt())
            setSingleLine(true)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            addTextChangedListener(object : TextWatcher {
                override fun afterTextChanged(s: Editable?) { filterProcessList(s?.toString().orEmpty()) }
                override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) {}
                override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) {}
            })
        }
        topRow.addView(procSearchEdit)

        procSysBtn = button(ctx, "仅应用") {
            procIncludeSystem = !procIncludeSystem
            procSysBtn.text = if (procIncludeSystem) "含系统" else "仅应用"
            refreshProcesses()
        }
        topRow.addView(procSysBtn)
        topRow.addView(button(ctx, "刷新") { refreshProcesses() })
        processBox.addView(topRow)

        procListView = ListView(ctx).apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
        }
        processBox.addView(procListView)

        return processBox
    }

    private fun refreshProcesses() {
        val ctx = this
        scope.launch {
            val list = EngineRepo.listProcesses(ctx, procIncludeSystem)
            procList = list
            filterProcessList(procSearchEdit.text.toString())
        }
    }

    private fun filterProcessList(query: String) {
        val ctx = this
        val q = query.trim().lowercase()
        val filtered = if (q.isEmpty()) procList else {
            procList.filter {
                it.name.lowercase().contains(q) || it.label.lowercase().contains(q) || it.pid.toString().contains(q)
            }
        }
        procListView.adapter = object : BaseAdapter() {
            override fun getCount(): Int = filtered.size
            override fun getItem(position: Int): Any = filtered[position]
            override fun getItemId(position: Int): Long = filtered[position].pid.toLong()
            override fun getView(position: Int, convertView: View?, parent: ViewGroup?): View {
                val item = filtered[position]
                val layout = (convertView as? LinearLayout) ?: LinearLayout(ctx).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                    setPadding(dp(8), dp(6), dp(8), dp(6))
                }
                layout.removeAllViews()

                val infoLayout = LinearLayout(ctx).apply {
                    orientation = LinearLayout.VERTICAL
                    layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                }

                val titleRow = LinearLayout(ctx).apply {
                    orientation = LinearLayout.HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                }

                if (item.foreground) {
                    titleRow.addView(TextView(ctx).apply {
                        text = "前台"
                        textSize = 9f
                        setTextColor(Color.WHITE)
                        typeface = Typeface.DEFAULT_BOLD
                        background = GradientDrawable().apply {
                            setColor(0xFFE91E63.toInt())
                            cornerRadius = dp(3).toFloat()
                        }
                        setPadding(dp(4), dp(1), dp(4), dp(1))
                    })
                }

                val attachedPid = EngineRepo.attached.value?.pid
                if (attachedPid == item.pid) {
                    titleRow.addView(TextView(ctx).apply {
                        text = "已附加"
                        textSize = 9f
                        setTextColor(Color.WHITE)
                        typeface = Typeface.DEFAULT_BOLD
                        background = GradientDrawable().apply {
                            setColor(0xFF4CAF50.toInt())
                            cornerRadius = dp(3).toFloat()
                        }
                        setPadding(dp(4), dp(1), dp(4), dp(1))
                        (layoutParams as? LinearLayout.LayoutParams ?: LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT)).apply {
                            setMargins(dp(3), 0, 0, 0)
                        }
                    })
                }

                titleRow.addView(TextView(ctx).apply {
                    text = item.label.ifEmpty { item.name }
                    textSize = 13f
                    setTextColor(Color.WHITE)
                    typeface = Typeface.DEFAULT_BOLD
                    setPadding(dp(4), 0, 0, 0)
                })
                infoLayout.addView(titleRow)

                val subText = TextView(ctx).apply {
                    text = "${item.name}  pid=${item.pid}"
                    textSize = 10f
                    setTextColor(0xFF9E9E9E.toInt())
                }
                infoLayout.addView(subText)
                layout.addView(infoLayout)

                val attachBtn = Button(ctx).apply {
                    text = if (attachedPid == item.pid) "脱离" else "附加"
                    textSize = 11f
                    setTextColor(Color.WHITE)
                    minWidth = 0
                    minimumWidth = 0
                    setPadding(dp(10), 0, dp(10), 0)
                    isAllCaps = false
                    layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(32))
                    setOnClickListener {
                        if (attachedPid == item.pid) {
                            EngineRepo.launch { EngineRepo.detach() }
                        } else {
                            EngineRepo.launch {
                                EngineRepo.attach(item.pid)
                            }
                            switchTab(TAB_SEARCH)
                        }
                    }
                }
                layout.addView(attachBtn)

                layout.setOnClickListener {
                    EngineRepo.launch { EngineRepo.attach(item.pid) }
                    switchTab(TAB_SEARCH)
                }

                return layout
            }
        }
    }

    // ---------- 冻结页 ----------

    private fun buildFrozenContent(): View {
        val ctx = this
        frozenBox = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
        }

        val topRow = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        frozenCountText = TextView(ctx).apply {
            text = "冻结列表: 0 项"
            textSize = 12f
            setTextColor(Color.WHITE)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        topRow.addView(frozenCountText)
        topRow.addView(button(ctx, "全部解冻") {
            EngineRepo.launch {
                EngineRepo.unfreezeAll()
                refreshFrozenList()
            }
        })
        topRow.addView(button(ctx, "刷新") { refreshFrozenList() })
        frozenBox.addView(topRow)

        frozenListView = ListView(ctx).apply {
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
        }
        frozenBox.addView(frozenListView)

        return frozenBox
    }

    private fun refreshFrozenList() {
        val ctx = this
        scope.launch {
            val list = EngineRepo.frozenList()
            frozenItems = list
            frozenCountText.text = "冻结列表: ${list.size} 项"
            frozenListView.adapter = object : BaseAdapter() {
                override fun getCount(): Int = list.size
                override fun getItem(position: Int): Any = list[position]
                override fun getItemId(position: Int): Long = list[position].addr
                override fun getView(position: Int, convertView: View?, parent: ViewGroup?): View {
                    val item = list[position]
                    val layout = (convertView as? LinearLayout) ?: LinearLayout(ctx).apply {
                        orientation = LinearLayout.HORIZONTAL
                        gravity = Gravity.CENTER_VERTICAL
                        setPadding(dp(8), dp(6), dp(8), dp(6))
                    }
                    layout.removeAllViews()

                    val tv = TextView(ctx).apply {
                        val valStr = if (item.type in setOf("f32", "f64")) "%.3f".format(item.value) else item.value.toLong().toString()
                        text = "0x%016X  %s (%s)".format(item.addr, valStr, item.type)
                        textSize = 12f
                        setTextColor(Color.WHITE)
                        layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
                    }
                    layout.addView(tv)

                    val unfreezeBtn = Button(ctx).apply {
                        text = "解冻"
                        textSize = 11f
                        setTextColor(Color.WHITE)
                        minWidth = 0
                        minimumWidth = 0
                        setPadding(dp(8), 0, dp(8), 0)
                        layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(32))
                        setOnClickListener {
                            EngineRepo.launch {
                                EngineRepo.unfreeze(item.addr)
                                refreshFrozenList()
                            }
                        }
                    }
                    layout.addView(unfreezeBtn)
                    return layout
                }
            }
        }
    }

    // ---------- AI 页 ----------

    private fun buildAiContent(): View {
        val ctx = this
        aiBox = LinearLayout(ctx).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
        }

        val topRow = LinearLayout(ctx).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        aiStatusText = TextView(ctx).apply {
            text = "智能体工具活动"
            textSize = 12f
            setTextColor(ACCENT)
            layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        }
        topRow.addView(aiStatusText)
        topRow.addView(button(ctx, "清空") {
            aiItems.clear()
            aiAdapter.notifyDataSetChanged()
        })
        aiBox.addView(topRow)

        aiAdapter = ArrayAdapter(ctx, android.R.layout.simple_list_item_1, aiItems)
        aiListView = ListView(ctx).apply {
            adapter = aiAdapter
            layoutParams = LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f)
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
        setPadding(dp(8), 0, dp(8), 0)
        setTextColor(Color.WHITE)
        layoutParams = LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.WRAP_CONTENT, dp(34)
        ).apply { setMargins(dp(2), 0, 0, 0) }
        setOnClickListener { onClick() }
    }

    // ---------- 数据订阅 ----------

    private var collectorsStarted = false

    private fun registerFlowCollectors() {
        if (collectorsStarted) return
        collectorsStarted = true

        scope.launch {
            EngineRepo.attached.collect { a ->
                if (a != null) {
                    val meta = listOfNotNull(a.arch.ifEmpty { null }, a.engine.ifEmpty { null }).joinToString(" · ")
                    statusText.text = if (meta.isNotEmpty()) "🟢 目标: ${a.name} (pid=${a.pid} | $meta)"
                    else "🟢 目标: ${a.name} (pid=${a.pid})"
                } else {
                    statusText.text = "🔴 未附加（点击此处选择进程）"
                }
                statusText.setTextColor(if (a != null) 0xFF81C784.toInt() else 0xFFE57373.toInt())
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
                searchCountText.text = "$t 条"
                aiStatusText.text = if (EngineRepo.scanning.value) "AI 操作中…" else "智能体活动流"
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
