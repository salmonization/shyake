/* shyake GUI frontend — plain JS, no build step. */

const TOKEN = window.__SHYAKE_TOKEN__;
const $ = (id) => document.getElementById(id);

let state = { box: "inbox", currentMail: null };

async function api(path, opts = {}) {
    const res = await fetch("/api" + path, {
        ...opts,
        headers: {
            "x-shyake-token": TOKEN,
            ...(opts.body ? { "content-type": "application/json" } : {}),
        },
    });
    const data = await res.json().catch(() => ({}));
    if (!res.ok) {
        const e = new Error(data.error || `HTTP ${res.status}`);
        e.code = data.code;
        e.status = res.status;
        throw e;
    }
    return data;
}

function post(path, body) {
    return api(path, { method: "POST", body: JSON.stringify(body ?? {}) });
}

function showMsg(el, text, ok = false) {
    el.textContent = text || "";
    el.className = "msg" + (text ? (ok ? " ok" : " error") : "");
}

function fmtTime(ts) {
    if (!ts) return "";
    const d = new Date(ts * 1000);
    const pad = (n) => String(n).padStart(2, "0");
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ` +
           `${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

function fmtSize(n) {
    if (n < 1024) return n + " B";
    if (n < 1024 * 1024) return (n / 1024).toFixed(1) + " KB";
    return (n / 1024 / 1024).toFixed(1) + " MB";
}

/* --- view switching --- */

function show(view) {
    for (const v of ["setup", "unlock", "main"])
        $("view-" + v).classList.toggle("hidden", v !== view);
}

async function boot() {
    const st = await api("/status");
    if (!st.initialized || !st.registered) {
        show("setup");
    } else if (!st.unlocked) {
        $("unlock-ident").textContent =
            `${st.username ?? "?"} @ ${st.instance ?? "?"}`;
        $("unlock-pass-wrap").classList.toggle("hidden", !st.keyEncrypted);
        show("unlock");
        if (!st.keyEncrypted) unlock("");
    } else {
        enterMain(st);
    }
}

function enterMain(st) {
    $("main-ident").textContent = `${st.username} @ ${st.instance}`;
    $("acct-ident").textContent =
        `${st.username} @ ${st.instance} · 配置目录 ${st.configDir}`;
    show("main");
    refreshBox("inbox");
    loadBlocklist();
}

/* --- setup --- */

$("btn-setup").onclick = async () => {
    const instance = $("setup-instance").value.trim() || "https://shyake.eee.coffee";
    const username = $("setup-username").value.trim();
    const p1 = $("setup-pass1").value;
    const p2 = $("setup-pass2").value;
    const msg = $("setup-msg");
    if (!username) return showMsg(msg, "请输入用户名");
    if (p1 !== p2) return showMsg(msg, "两次输入的密码不一致");
    $("btn-setup").disabled = true;
    showMsg(msg, "正在生成密钥并注册（可能需要几十秒）…", true);
    try {
        await post("/setup", { instance, username, passphrase: p1 });
        boot();
    } catch (e) {
        showMsg(msg, e.message);
    } finally {
        $("btn-setup").disabled = false;
    }
};

/* --- unlock / lock --- */

async function unlock(passphrase) {
    const msg = $("unlock-msg");
    showMsg(msg, "解锁中…", true);
    try {
        await post("/unlock", { passphrase });
        boot();
    } catch (e) {
        showMsg(msg, e.message);
    }
}

$("btn-unlock").onclick = () => unlock($("unlock-pass").value);
$("unlock-pass").addEventListener("keydown", (e) => {
    if (e.key === "Enter") unlock($("unlock-pass").value);
});

$("btn-lock").onclick = async () => {
    await post("/lock");
    location.reload();
};

/* --- tabs --- */

document.querySelectorAll("nav.tabs button").forEach((b) => {
    b.onclick = () => {
        document.querySelectorAll("nav.tabs button")
            .forEach((x) => x.classList.toggle("active", x === b));
        document.querySelectorAll(".tabpane")
            .forEach((p) => p.classList.toggle("hidden", p.id !== "tab-" + b.dataset.tab));
        if (["inbox", "sent", "saved"].includes(b.dataset.tab))
            refreshBox(b.dataset.tab);
    };
});

document.querySelectorAll("[data-refresh]").forEach((b) => {
    b.onclick = () => refreshBox(b.dataset.refresh);
});

/* --- mail lists --- */

async function refreshBox(box) {
    const list = $("list-" + box);
    const msg = $("msg-" + box);
    showMsg(msg, "加载中…", true);
    try {
        const items = await api("/mail/" + box);
        showMsg(msg, "");
        renderList(box, items);
    } catch (e) {
        showMsg(msg, e.message);
        list.innerHTML = "";
    }
}

function renderList(box, items) {
    const el = $("list-" + box);
    if (!items.length) {
        el.innerHTML = '<div class="empty">暂无邮件</div>';
        return;
    }
    const partyLabel = box === "inbox" || box === "saved" ? "发件人" : "收件人";
    const rows = items.map((m) => `
        <tr data-id="${encodeURIComponent(m.mail_id ?? "")}" data-box="${box}"
            data-party="${escapeHtml(m.party ?? "")}">
            <td class="meta">${fmtTime(m.timestamp)}</td>
            <td>${escapeHtml(m.party ?? "?")}</td>
            <td class="subj">${escapeHtml(m.subject ?? "(解密失败)")}</td>
            <td class="meta">${fmtSize(m.size)}</td>
        </tr>`).join("");
    el.innerHTML = `
        <table class="mail">
            <thead><tr><th>时间</th><th>${partyLabel}</th><th>主题</th><th>大小</th></tr></thead>
            <tbody>${rows}</tbody>
        </table>`;
    el.querySelectorAll("tbody tr").forEach((tr) => {
        tr.onclick = () =>
            openMail(decodeURIComponent(tr.dataset.id), tr.dataset.box, tr.dataset.party);
    });
}

function escapeHtml(s) {
    return s.replace(/[&<>"']/g, (c) =>
        ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

/* --- mail detail modal --- */

async function openMail(id, box, party) {
    $("overlay").classList.remove("hidden");
    $("mail-subject").textContent = "加载中…";
    $("mail-meta").textContent = "";
    $("mail-body").textContent = "";
    showMsg($("modal-msg"), "");
    const isSaved = box === "saved";
    $("btn-reply").classList.toggle("hidden", box !== "inbox");
    $("btn-save-mail").classList.toggle("hidden", isSaved);
    $("btn-block-sender").classList.toggle("hidden", box !== "inbox");
    $("btn-burn").classList.toggle("hidden", isSaved);
    try {
        const d = await api("/mail/detail/" + encodeURIComponent(id) +
                            (isSaved ? "?src=saved" : ""));
        state.currentMail = { ...d, box };
        $("mail-subject").textContent = d.subject ?? "(无主题)";
        $("mail-meta").textContent =
            `${d.sender ?? "?"} → ${d.recipient ?? "?"} · ${fmtTime(d.timestamp)} · ` +
            `${fmtSize(d.size)} · ID ${d.mail_id}`;
        $("mail-body").textContent = d.body ?? "(正文解密失败)";
    } catch (e) {
        $("mail-subject").textContent = "加载失败";
        showMsg($("modal-msg"), e.message);
    }
}

$("btn-close-modal").onclick = () => {
    $("overlay").classList.add("hidden");
    state.currentMail = null;
};

$("btn-reply").onclick = () => {
    const m = state.currentMail;
    if (!m) return;
    $("btn-close-modal").onclick();
    document.querySelector('nav.tabs button[data-tab="compose"]').click();
    $("compose-to").value = m.sender ?? "";
    $("compose-subject").value = "Re: " + (m.subject ?? "");
    $("compose-body").value = "";
    $("compose-body").focus();
};

$("btn-save-mail").onclick = async () => {
    const m = state.currentMail;
    if (!m) return;
    try {
        await post("/save/" + encodeURIComponent(m.mail_id));
        showMsg($("modal-msg"), "已保存到本地", true);
    } catch (e) {
        showMsg($("modal-msg"), e.message);
    }
};

$("btn-block-sender").onclick = async () => {
    const m = state.currentMail;
    if (!m?.sender) return;
    if (!confirm(`确定拉黑 ${m.sender}？`)) return;
    try {
        await post("/block", { target: m.sender });
        showMsg($("modal-msg"), `已拉黑 ${m.sender}`, true);
        loadBlocklist();
    } catch (e) {
        showMsg($("modal-msg"), e.message);
    }
};

$("btn-burn").onclick = async () => {
    const m = state.currentMail;
    if (!m) return;
    if (!confirm("确定删除这封邮件？此操作不可撤销。")) return;
    try {
        await post("/burn/" + encodeURIComponent(m.mail_id));
        $("btn-close-modal").onclick();
        refreshBox(m.box);
    } catch (e) {
        showMsg($("modal-msg"), e.message);
    }
};

/* --- compose --- */

$("btn-send").onclick = async () => {
    const to = $("compose-to").value.trim();
    const subject = $("compose-subject").value.trim();
    const body = $("compose-body").value;
    const msg = $("compose-msg");
    if (!to || !subject) return showMsg(msg, "收件人和主题不能为空");
    $("btn-send").disabled = true;
    showMsg(msg, "发送中…", true);
    try {
        await post("/send", { to, subject, body });
        showMsg(msg, "已发送", true);
        $("compose-to").value = "";
        $("compose-subject").value = "";
        $("compose-body").value = "";
    } catch (e) {
        showMsg(msg, e.message +
            (e.code === -4 ? "（对方公钥已变更，请在“账户”页核实指纹后更新信任）" : ""));
    } finally {
        $("btn-send").disabled = false;
    }
};

/* --- account --- */

$("btn-self-fp").onclick = async () => {
    try {
        const fp = await api("/fingerprint");
        const el = $("acct-fp");
        el.textContent = fp.remote_fp;
        el.classList.remove("hidden");
    } catch (e) {
        showMsg($("acct-msg"), e.message);
    }
};

$("btn-fp-lookup").onclick = async () => {
    const user = $("fp-user").value.trim();
    if (!user) return;
    try {
        const fp = await api("/fingerprint?user=" + encodeURIComponent(user));
        const el = $("fp-result");
        let text = `远程指纹: ${fp.remote_fp}`;
        if (fp.has_local) {
            text += `\n本地记录: ${fp.local_fp}\n状态: ` +
                (fp.match ? "一致 ✓" : "不一致 ✗（核实后可更新信任）");
        } else {
            text += "\n本地无记录（首次联系）";
        }
        el.textContent = text;
        el.classList.remove("hidden");
        $("btn-fp-update").classList.toggle("hidden", fp.has_local && fp.match);
    } catch (e) {
        showMsg($("acct-msg"), e.message);
    }
};

$("btn-fp-update").onclick = async () => {
    const user = $("fp-user").value.trim();
    if (!user) return;
    if (!confirm(`确认已带外核实 ${user} 的新指纹？`)) return;
    try {
        await post("/fingerprint/update", { user });
        showMsg($("acct-msg"), `已更新 ${user} 的信任记录`, true);
        $("btn-fp-update").classList.add("hidden");
    } catch (e) {
        showMsg($("acct-msg"), e.message);
    }
};

$("btn-block").onclick = async () => {
    const target = $("block-target").value.trim();
    if (!target) return;
    try {
        await post("/block", { target });
        $("block-target").value = "";
        loadBlocklist();
    } catch (e) {
        showMsg($("acct-msg"), e.message);
    }
};

async function loadBlocklist() {
    try {
        const items = await api("/blocklist");
        const ul = $("block-list");
        if (!items.length) {
            ul.innerHTML = '<li><span style="color:var(--dim)">空</span></li>';
            return;
        }
        ul.innerHTML = items.map((b) => `
            <li><span>${escapeHtml(b.target ?? "?")}</span>
            <button class="ghost" data-unblock="${escapeHtml(b.target ?? "")}">解除</button></li>
        `).join("");
        ul.querySelectorAll("[data-unblock]").forEach((b) => {
            b.onclick = async () => {
                await post("/block", { target: b.dataset.unblock, unblock: true });
                loadBlocklist();
            };
        });
    } catch { /* locked or offline */ }
}

$("btn-rotate").onclick = async () => {
    if (!confirm("轮换密钥会清空与你相关的所有邮件，继续？")) return;
    try {
        await post("/rotate", { newPassphrase: $("rotate-pass").value });
        showMsg($("acct-msg"), "密钥已轮换", true);
        $("rotate-pass").value = "";
    } catch (e) {
        showMsg($("acct-msg"), e.message);
    }
};

$("btn-destroy").onclick = async () => {
    const confirmName = $("destroy-confirm").value.trim();
    if (!confirmName) return showMsg($("acct-msg"), "请输入用户名以确认");
    if (!confirm("此操作永久销毁账户且不可恢复，继续？")) return;
    try {
        await post("/destroy", { confirm: confirmName });
        location.reload();
    } catch (e) {
        showMsg($("acct-msg"), e.message);
    }
};

boot().catch((e) => alert("初始化失败: " + e.message));
