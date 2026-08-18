(async function main() {
  const titles = {
    dashboard: "Dashboard",
    beds: "Beds & Rooms",
    alerts: "Alerts",
    devices: "Devices",
    "system-log": "System Log",
    profile: "My shift",
    users: "Users",
    faults: "Fault reports",
    directory: "Bed directory"
  };

  /* Where each role lands. Everyone used to arrive at the Dashboard; for a
   * technician or an administrator that tab no longer exists, so without this
   * they would open the console to a blank page. Ordered by capability, not by
   * role name, so a new role inherits a sensible landing tab for free. */
  const LANDING = [
    ["ward.view", "dashboard"],
    ["devices.manage", "devices"],
    ["users.manage", "users"],
    ["logs.view", "system-log"]
  ];

  /* Tabs that fetch on demand instead of riding the live SignalR feed. They
   * are told when they become visible so they do not poll in the background
   * for a screen nobody is looking at. */
  const ON_DEMAND = {
    profile: ProfileTab,
    users: UsersTab,
    faults: FaultsTab,
    directory: DirectoryTab
  };

  // Remembers the last tab across a reload, so an administrator editing the
  // Bed directory (or anyone else mid-task) does not land back on their
  // role's default tab every time the page refreshes.
  const LAST_TAB_KEY = "his.lastTab";

  function switchTab(tab) {
    document.querySelectorAll(".nav-btn[data-tab]").forEach((btn) => {
      btn.classList.toggle("active", btn.getAttribute("data-tab") === tab);
    });
    document.querySelectorAll(".tab-panel").forEach((panel) => {
      panel.classList.toggle("active", panel.id === `tab-${tab}`);
    });
    document.getElementById("pageTitle").textContent = titles[tab] || tab;

    Object.entries(ON_DEMAND).forEach(([name, module]) => {
      if (name === tab) module.activate();
      else module.stop?.();
    });

    localStorage.setItem(LAST_TAB_KEY, tab);
  }

  document.querySelectorAll(".nav-btn[data-tab]").forEach((btn) => {
    btn.addEventListener("click", () => switchTab(btn.getAttribute("data-tab")));
  });

  document.querySelectorAll("[data-close-modal]").forEach((btn) => {
    btn.addEventListener("click", () => {
      document.getElementById(btn.getAttribute("data-close-modal")).classList.add("hidden");
    });
  });

  /* Identity first: every module below asks Session.can() while rendering, so
   * the answer has to exist before any of them draw anything. A failed load
   * has already redirected to the login page. */
  const me = await Session.load();
  if (!me) return;

  Session.renderIdentity();
  Session.applyTo(document);
  document.getElementById("logoutBtn")?.addEventListener("click", () => Session.logout());

  /* Bed data comes from one of two endpoints depending on the role: the full
   * one with vitals for the ward, or the directory (identifiers and rooms
   * only) for the technician assigning devices. Asking for the wrong one is a
   * 403, not a silent empty screen. */
  try {
    if (Session.can(Session.CAP.viewWard)) {
      State.setBeds(await Api.getBeds());
    } else if (Session.can(Session.CAP.viewBedDirectory)) {
      const directory = await Api.getBedDirectory();
      State.setBeds(directory.map((b) => ({ bedId: b.bedId, room: b.room, status: "Unknown" })));
    }
  } catch (err) {
    console.error("Failed to load initial bed data:", err);
  }

  /* Ward modules only exist for roles that can see the ward - their DOM is
   * removed for everyone else, and initialising them would throw on the first
   * missing element. */
  if (Session.can(Session.CAP.viewWard)) {
    DashboardTab.init();
    BedsTab.init();
    AlertsTab.init();
  }
  if (Session.can(Session.CAP.handleFaults)) FaultsTab.init();
  // These two tabs are removed entirely for roles without the capability, so
  // their modules must not be initialised either - their DOM is gone.
  if (Session.can(Session.CAP.manageDevices)) DevicesTab.init();
  if (Session.can(Session.CAP.viewLogs)) SystemLogTab.init();
  // Last, so the tab modules exist by the time it can open a bed. Clinical
  // alarms are for clinical roles: a technician has no way to act on one.
  if (Session.can(Session.CAP.viewWard)) CriticalAlarm.init();

  // Land back on whatever tab was open before the reload, as long as this
  // role still has a nav button for it (Session.applyTo above already
  // removed any the role cannot use). Otherwise fall back to the first tab
  // this role can actually use.
  const lastTab = localStorage.getItem(LAST_TAB_KEY);
  const lastTabAllowed = lastTab && document.querySelector(`.nav-btn[data-tab="${lastTab}"]`);
  if (lastTabAllowed) {
    switchTab(lastTab);
  } else {
    const landing = LANDING.find(([cap]) => Session.can(cap));
    if (landing) switchTab(landing[1]);
  }

  await MonitoringHub.start();

  /* A password an administrator typed is not one the owner has chosen. The
   * dialog has no cancel, so the console stays unusable until it is replaced. */
  if (me.mustChangePassword) {
    Session.promptChangePassword({ forced: true });
  }
})();
