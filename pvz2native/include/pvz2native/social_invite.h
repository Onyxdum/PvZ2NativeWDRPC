#pragma once
/* Invite a friend through Discord.
 *
 * The world map's social button (the envelope with a "+", 4.5.2 only) opened the game's Facebook friends
 * dialog, which cannot connect here. It now copies an invite link to the clipboard instead: the cloud
 * server's /invite page, which shows the player's Discord name and picture and links to the download.
 * The same link is the second button of the Discord Rich Presence. */

#include <string>

namespace pvz2native {

/* <cloud server>/invite, plus ?from=<Discord id>&n=<name> when a Discord account is linked. */
std::string invite_url();

/* Copy invite_url() to the clipboard and show a notification. Safe from the guest thread: the work is
 * done on the next frame by the config screen. */
void invite_button_clicked();

}  // namespace pvz2native
