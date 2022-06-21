{{admon/important | Comments and Explanations | The page source contains comments providing guidance to fill out each section. They are invisible when viewing this page. To read it, choose the "view source" link.<br/> '''Copy the source to a ''new page'' before making changes!  DO NOT EDIT THIS TEMPLATE FOR YOUR CHANGE PROPOSAL.'''}}

{{admon/tip | Guidance | For details on how to fill out this form, see the [https://docs.fedoraproject.org/en-US/program_management/changes_guide/ documentation].}}

<!-- The actual name of your proposed change page should look something like: Changes/Your_Change_Proposal_Name.  This keeps all change proposals in the same namespace -->

= MAC Address Policy none <!-- The name of your change proposal --> =

{{Change_Proposal_Banner}}

== Summary ==
<!-- A sentence or two summarizing what this change is and what it will do. This information is used for the overall changeset summary page for each release. Note that motivation for the change should be in the Benefit to Fedora section below, and this part should answer the question "What?" rather than "Why?". -->

systemd-udev package installs "/usr/lib/systemd/network/99-default.link",
which sets `Link.MACAddressPolicy=persistent`. This proposal is to change
that and set Link.MACAddressPolicy=none to stop changing the MAC address.

== Owner ==
<!-- 
For change proposals to qualify as self-contained, owners of all affected packages need to be included here. Alternatively, a SIG can be listed as an owner if it owns all affected packages. 
This should link to your home wiki page so we know who you are. 
-->
<!-- Include you email address that you can be reached should people want to contact you about helping with your change, status is requested, or technical issues need to be resolved. If the change proposal is owned by a SIG, please also add a primary contact person. -->
* Name: [[User:thaller|Thomas Haller]]
* Email: <thaller@redhat.com>
* Name: [[User:dustymabe| Dusty Mabe]] (Fedora CoreOS)
* Email: <dmabe@redhat.com>

<!--- UNCOMMENT only for Changes with assigned Shepherd (by FESCo)
* FESCo shepherd: [[User:FASAccountName| Shehperd name]] <email address>
-->


== Current status ==
[[Category:ChangePageIncomplete]]
<!-- When your change proposal page is completed and ready for review and announcement -->
<!-- remove Category:ChangePageIncomplete and change it to Category:ChangeReadyForWrangler -->
<!-- The Wrangler announces the Change to the devel-announce list and changes the category to Category:ChangeAnnounced (no action required) --> 
<!-- After review, the Wrangler will move your page to Category:ChangeReadyForFesco... if it still needs more work it will move back to Category:ChangePageIncomplete-->

<!-- Select proper category, default is Self Contained Change -->
<!-- [[Category:SelfContainedChange]] -->
[[Category:SystemWideChange]]

* Targeted release: [[Releases/37 | Fedora Linux 37 ]] 
* Last updated: <!-- this is an automatic macro — you don't need to change this line -->  {{REVISIONYEAR}}-{{REVISIONMONTH}}-{{REVISIONDAY2}} 
<!-- After the change proposal is accepted by FESCo, tracking bug is created in Bugzilla and linked to this page 
Bugzilla state meanings:
ASSIGNED -> accepted by FESCo with ongoing development
MODIFIED -> change is substantially done and testable
ON_QA -> change is fully code complete
-->
* FESCo issue: <will be assigned by the Wrangler>
* Tracker bug: <will be assigned by the Wrangler>
* Release notes tracker: <will be assigned by the Wrangler>

== Detailed Description ==
<!-- Expand on the summary, if appropriate.  A couple sentences suffices to explain the goal, but the more details you can provide the better. -->

On Fedora, udev by default changes the MAC address of a wide range of software devices.
This was introduced by systemd 242 in sprint 2019 (Fedora 31), when MACAddressPolicy was
extended to affect more types of devices.

Udev's aim here is to provide a stable MAC address, otherwise kernel will assign a random one.
However, that can cause problems:

- Software devices are always created by some tool that has plans for the device. The tool may not
  expect that udev is going to change the MAC address and races against that. The best solution
  for the tool is to set the MAC address when creating an interface. This will prevent
  udev from changing the MAC address according to the MACAddressPolicy.
  Otherwise, the tool should wait for udev to initialize the device to avoid the race. In theory, a
  tool is always advised to wait for udev to initialize the device. However, if it were not for MACAddressPolicy,
  in common scenarios udev doesn't do anything relevant for software devices to make that necessary.

- for interface types bridge and bond, an unset MAC address has a special meaning
  to kernel and the MAC address of the first port is used. If udev changes the MAC
  address, that no longer works.

  The generated MAC address is not directly discoverable as it is based on `/etc/machine-id`
  ([machine-id(5)](https://www.man7.org/linux/man-pages/man5/machine-id.5.html)), among
  other data. Even if there were a tool to easily calculate the MAC address, it could be cumbersome
  to use it without logging into the machine first. The MAC address can directly affect the
  assigned IP address, for example when using DHCP. When booting a new virtual machine, the user might
  know the MAC address of the (virtual) "physical" interfaces. When bonding/briding those
  interfaces, the bond/bridge would get one of the well known MAC addresses. MACAddressPolicy=persistent
  interferes with that.

The goal of persistent policy is to provide a stable MAC address. Note that if the
tool or user who created the interface would want a certain MAC address, they
have all the means to set it already. That applies regardless whether the tool is
iproute2, NetworkManager, systemd-networkd. Neither NetworkManager nor systemd-networkd
rely on udev's MACAddressPolicy for setting the MAC address. This behavior is mostly
useful for plain `ip link add`, but it's unclear which real world user wants this behavior.

Of course, the user is welcome to configure the MAC address in any way they want. Including,
dropping a link file that sets MACAddressPolicy=persistent. The problem is once udev sets a MAC address,
it cannot be unset. Which makes this problematic to do by default.

While Fedora inherited this behavior from upstream systemd, RHEL-9 does not follow this behavior
([[centos9]](https://gitlab.com/redhat/centos-stream/rpms/systemd/-/blob/c8953519504bf2e694bfbc2b02a456c1056f252e/0028-udev-net-setup-link-change-the-default-MACAddressPol.patch#L43),
[[rh#1921094]](https://bugzilla.redhat.com/show_bug.cgi?id=1921094)). RHEL-8's systemd is too old to
change the MAC address of most software devices.

This could be either implemented by patching `/usr/lib/systemd/network/99-default.link`
to have a different policy, or by dropping a link file with higher priority. In the latter
case, that override could be shipped either by udev or even by NetworkManager.
The override could also limit `MACAddressPolicy=none` to certain device types only,
like bridge, bond and team interfaces.


== Feedback ==
<!-- Summarize the feedback from the community and address why you chose not to accept proposed alternatives. This section is optional for all change proposals but is strongly suggested. Incorporating feedback here as it is raised gives FESCo a clearer view of your proposal and leaves a good record for the future. If you get no feedback, that is useful to note in this section as well. For innovative or possibly controversial ideas, consider collecting feedback before you file the change proposal. -->

This was also discussed on upstream systemd mailing list [here](https://lists.freedesktop.org/archives/systemd-devel/2022-May/047893.html).
The upstream systemd maintainers' opinion is that the current udev behavior is desirable.

The RHEL-9 bug is [here](https://bugzilla.redhat.com/show_bug.cgi?id=1921094).

== Benefit to Fedora ==
<!-- What is the benefit to the distribution?  Will the software we generate be improved? How will the process of creating Fedora releases be improved?
  
      Be sure to include the following areas if relevant:
      If this is a major capability update, what has changed?
           For example: This change introduces Python 5 that runs without the Global Interpreter Lock and is fully multithreaded.
      If this is a new functionality, what capabilities does it bring?
           For example: This change allows package upgrades to be performed automatically and rolled-back at will.
      Does this improve some specific package or set of packages?
           For example: This change modifies a package to use a different language stack that reduces install size by removing dependencies.
      Does this improve specific Spins or Editions?
           For example: This change modifies the default install of Fedora Workstation to be more in line with the base install of Fedora Server.
      Does this make the distribution more efficient?
           For example: This change replaces thousands of individual %post scriptlets in packages with one script that runs at the end.
      Is this an improvement to maintainer processes?
           For example: Gating Fedora packages on automatic QA tests will make rawhide more stable and allow changes to be implemented more smoothly.
      Is this an improvement targeted as specific contributors?
           For example: Ensuring that a minimal set of tools required for contribution to Fedora are installed by default eases the onboarding of new contributors. 

     When a Change has multiple benefits, it's better to list them all.

     Consider these Change pages from previous editions as inspiration:
     https://fedoraproject.org/wiki/Changes/Annobin (low-level and technical, invisible to users)
     https://fedoraproject.org/wiki/Changes/ParallelInstallableDebuginfo (low-level, but visible to advanced users)
     https://fedoraproject.org/wiki/Changes/VirtualBox_Guest_Integration (primarily a UX change)
     https://fedoraproject.org/wiki/Changes/NoMoreAlpha (an improvement to distro processes)
     https://fedoraproject.org/wiki/Changes/perl5.26 (major upgrade to a popular software stack, visible to users of that stack)
-->

Pros:

- Consistent behavior with RHEL8 and RHEL9.

- Bridge and bond interfaces can get the MAC addresses from their first port.

- avoid race of udev and the tool that creates the interface.

Cons:

- deviate from upstream systemd.

It is desirable that RHEL and Fedora behaves similar. A possibly outcome
could be the current behavior stays and RHEL 10 would change behavior. On the
other hand, different distributions (or even Fedora spins) have different
uses and needs. Deviating might be fine. In the same vain, there is also
a desire to stay close to upstream systemd behavior. But the uses of systemd
project go beyond Fedora/RHEL, so deviating here may also be fine.


== Scope ==
* Proposal owners:
<!-- What work do the feature owners have to accomplish to complete the feature in time for release?  Is it a large change affecting many parts of the distribution or is it a very isolated change? What are those changes?-->

The main goal of this request is discussion and find the desired behavior.
The implementation/changes are either way very simple.


* Other developers: <!-- REQUIRED FOR SYSTEM WIDE CHANGES -->
<!-- What work do other developers have to accomplish to complete the feature in time for release?  Is it a large change affecting many parts of the distribution or is it a very isolated change? What are those changes?-->

Other projects that wish a certain MAC address are welcome to
set it for their devices. Including using udev's MACAddressPolicy.

* Release engineering: <!-- REQUIRED FOR SYSTEM WIDE CHANGES -->
<!-- Does this feature require coordination with release engineering (e.g. changes to installer image generation or update package delivery)?  Is a mass rebuild required?  include a link to the releng issue. 
The issue is required to be filed prior to feature submission, to ensure that someone is on board to do any process development work and testing and that all changes make it into the pipeline; a bullet point in a change is not sufficient communication -->

Not needed for this change.

* Policies and guidelines: N/A (not needed for this Change) <!-- REQUIRED FOR SYSTEM WIDE CHANGES -->
<!-- Do the packaging guidelines or other documents need to be updated for this feature?  If so, does it need to happen before or after the implementation is done?  If a FPC ticket exists, add a link here. Please submit a pull request with the proposed changes before submitting your Change proposal. -->

* Trademark approval: N/A (not needed for this Change)
<!-- If your Change may require trademark approval (for example, if it is a new Spin), file a ticket ( https://pagure.io/Fedora-Council/tickets/issues ) requesting trademark approval from the Fedora Council. This approval will be done via the Council's consensus-based process. -->

* Alignment with Objectives: 
<!-- Does your proposal align with the current Fedora Objectives: https://docs.fedoraproject.org/en-US/project/objectives/ ? It's okay if it doesn't, but it's something to consider -->

== Upgrade/compatibility impact ==
<!-- What happens to systems that have had a previous versions of Fedora installed and are updated to the version containing this change? Will anything require manual configuration or data migration? Will any existing functionality be no longer supported? -->

After the change, the MAC address for the affected device types changes.

<!-- REQUIRED FOR SYSTEM WIDE CHANGES -->


== How To Test ==
<!-- This does not need to be a full-fledged document. Describe the dimensions of tests that this change implementation is expected to pass when it is done.  If it needs to be tested with different hardware or software configurations, indicate them.  The more specific you can be, the better the community testing can be. 

Remember that you are writing this how to for interested testers to use to check out your change implementation - documenting what you do for testing is OK, but it's much better to document what *I* can do to test your change.

A good "how to test" should answer these four questions:

0. What special hardware / data / etc. is needed (if any)?
1. How do I prepare my system to test this change? What packages
need to be installed, config files edited, etc.?
2. What specific actions do I perform to check that the change is
working like it's supposed to?
3. What are the expected results of those actions?
-->

<!-- REQUIRED FOR SYSTEM WIDE CHANGES -->

1) Create a software device two times, for example `ip link add type bridge`. Note
that the MAC address is either stable or random, depending on the `MACAddressPolicy`.

2) Note that if the software device has the MAC address set initially, udev does not
change it (`ip link add address aa:aa:aa:aa:aa:aa type bridge`). That depends on
`addr_assign_type` sysctl.

3) Create a bridge/bond interface without setting the MAC address. Note that if `MACAddressPolicy=none`,
the MAC address is random at first. Note that attaching the first port will update the controller's MAC address.
On the other hand, with `MACAddressPolicy=persistent`, the MAC address of the controller is fixed
and not inherited from the port.

4) Run

```
  ip monitor link &
  while : ; do
    ip link del xxx
    ip link add name xxx type dummy \
    && ip link set xxx addr aa:00:00:00:00:00 \
    && ip link show xxx | grep -q aa:00:00:00:00:00 \
    || break
  done
```

to reproduce the race between a naive tool and udev changing the MAC address.


== User Experience ==
<!-- If this change proposal is noticeable by users, how will their experiences change as a result?

 This section partially overlaps with the Benefit to Fedora section above. This section should be primarily about the User Experience, written in a way that does not assume deep technical knowledge. More detailed technical description should be left for the Benefit to Fedora section.

 Describe what Users will see or notice, for example:
  - Packages are compressed more efficiently, making downloads and upgrades faster by 10%.
  - Kerberos tickets can be renewed automatically. Users will now have to authenticate less and become more productive. Credential management improvements mean a user can start their work day with a single sign on and not have to pause for reauthentication during their entire day.
 - Libreoffice is one of the most commonly installed applications on Fedora and it is now available by default to help users "hit the ground running".
 - Green has been scientifically proven to be the most relaxing color. The move to a default background color of green with green text will result in Fedora users being the most relaxed users of any operating system.
-->

The MAC address of software devices would again be random.


== Dependencies ==
<!-- What other packages (RPMs) depend on this package?  Are there changes outside the developers' control on which completion of this change depends?  In other words, completion of another change owned by someone else and might cause you to not be able to finish on time or that you would need to coordinate?  Other upstream projects like the kernel (if this is not a kernel change)? -->

<!-- REQUIRED FOR SYSTEM WIDE CHANGES -->

None.


== Contingency Plan ==

<!-- If you cannot complete your feature by the final development freeze, what is the backup plan?  This might be as simple as "Revert the shipped configuration".  Or it might not (e.g. rebuilding a number of dependent packages).  If you feature is not completed in time we want to assure others that other parts of Fedora will not be in jeopardy.  -->
* Contingency mechanism: (What to do?  Who will do it?) <!-- REQUIRED FOR SYSTEM WIDE CHANGES -->

If the change is rejected, nothing needs to be done. The change
itself will be simple to implement.

<!-- When is the last time the contingency mechanism can be put in place?  This will typically be the beta freeze. -->
* Contingency deadline: beta freeze  <!-- REQUIRED FOR SYSTEM WIDE CHANGES -->
<!-- Does finishing this feature block the release, or can we ship with the feature in incomplete state? -->
* Blocks release? No <!-- REQUIRED FOR SYSTEM WIDE CHANGES -->


== Documentation ==
<!-- Is there upstream documentation on this change, or notes you have written yourself?  Link to that material here so other interested developers can get involved. -->

<!-- REQUIRED FOR SYSTEM WIDE CHANGES -->
TODO.

== Release Notes ==
<!-- The Fedora Release Notes inform end-users about what is new in the release.  Examples of past release notes are here: http://docs.fedoraproject.org/release-notes/ -->
<!-- The release notes also help users know how to deal with platform changes such as ABIs/APIs, configuration or data file formats, or upgrade concerns.  If there are any such changes involved in this change, indicate them here.  A link to upstream documentation will often satisfy this need.  This information forms the basis of the release notes edited by the documentation team and shipped with the release. 

Release Notes are not required for initial draft of the Change Proposal but has to be completed by the Change Freeze. 
-->

