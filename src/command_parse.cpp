/*       +------------------------------------+
 *       | Inspire Internet Relay Chat Daemon |
 *       +------------------------------------+
 *
 *  InspIRCd is copyright (C) 2002-2006 ChatSpike-Dev.
 *                       E-mail:
 *                <brain@chatspike.net>
 *                <Craig@chatspike.net>
 *
 * Written by Craig Edwards, Craig McLure, and others.
 * This program is free but copyrighted software; see
 *            the file COPYING for details.
 *
 * ---------------------------------------------------
 */

#include "inspircd_config.h"
#include "inspircd.h"
#include "configreader.h"
#include <unistd.h>
#include <fcntl.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <time.h>
#include <string>
#include <sstream>
#include <vector>
#include <sched.h>
#ifdef THREADED_DNS
#include <pthread.h>
#endif
#include "users.h"
#include "globals.h"
#include "modules.h"
#include "dynamic.h"
#include "wildcard.h"
#include "message.h"
#include "mode.h"
#include "commands.h"
#include "xline.h"
#include "inspstring.h"
#include "dnsqueue.h"
#include "helperfuncs.h"
#include "hashcomp.h"
#include "socketengine.h"
#include "userprocess.h"
#include "socket.h"
#include "dns.h"
#include "typedefs.h"
#include "command_parse.h"
#include "ctables.h"

#define nspace __gnu_cxx

extern InspIRCd* ServerInstance;

extern std::vector<Module*> modules;
extern std::vector<ircd_module*> factory;
extern std::vector<InspSocket*> module_sockets;
extern std::vector<userrec*> local_users;

extern int MODCOUNT;
extern InspSocket* socket_ref[MAX_DESCRIPTORS];
extern time_t TIME;

// This table references users by file descriptor.
// its an array to make it VERY fast, as all lookups are referenced
// by an integer, meaning there is no need for a scan/search operation.
extern userrec* fd_ref_table[MAX_DESCRIPTORS];

extern Server* MyServer;
extern ServerConfig *Config;

extern user_hash clientlist;
extern chan_hash chanlist;

/* Special commands which may occur without registration of the user */
cmd_user* command_user;
cmd_nick* command_nick;
cmd_pass* command_pass;

/* This function pokes and hacks at a parameter list like the following:
 *
 * PART #winbot,#darkgalaxy :m00!
 *
 * to turn it into a series of individual calls like this:
 *
 * PART #winbot :m00!
 * PART #darkgalaxy :m00!
 *
 * The seperate calls are sent to a callback function provided by the caller
 * (the caller will usually call itself recursively). The callback function
 * must be a command handler. Calling this function on a line with no list causes
 * no action to be taken. You must provide a starting and ending parameter number
 * where the range of the list can be found, useful if you have a terminating
 * parameter as above which is actually not part of the list, or parameters
 * before the actual list as well. This code is used by many functions which
 * can function as "one to list" (see the RFC) */

int CommandParser::LoopCall(userrec* user, command_t* CommandObj, const char** parameters, int pcnt, unsigned int splithere, unsigned int extra)
{
	/*if ((unsigned int)j > Config->MaxTargets)
	{
		WriteServ(u->fd,"407 %s %s :Too many targets in list, message not delivered.",u->nick,sep_items[j-1]);
		return 1;
	}
	fn->Handle(pars,pcnt-(end-start),u);
	return 1;*/
	if (!strchr(parameters[splithere],','))
		return 0;

	irc::commasepstream items1(parameters[splithere]);
	irc::commasepstream items2(parameters[extra]);
	std::string item = "";

	while ((item = items1.GetToken()) != "")
	{
		std::string extrastuff = items2.GetToken();
		parameters[splithere] = item.c_str();
		parameters[extra] = extrastuff.c_str();
		CommandObj->Handle(parameters,pcnt,user);
	}
	return 1;
}

int CommandParser::LoopCall(userrec* user, command_t* CommandObj, const char** parameters, int pcnt, unsigned int splithere)
{
	if (!strchr(parameters[splithere],','))
		return 0;

	irc::commasepstream items1(parameters[splithere]);
	std::string item = "";

	while ((item = items1.GetToken()) != "")
	{
		parameters[splithere] = item.c_str();
		CommandObj->Handle(parameters,pcnt,user);
	}
	return 1;
}

bool CommandParser::IsValidCommand(const std::string &commandname, int pcnt, userrec * user)
{
	nspace::hash_map<std::string,command_t*>::iterator n = cmdlist.find(commandname);

	if (n != cmdlist.end())
	{
		if ((pcnt>=n->second->min_params) && (n->second->source != "<core>"))
		{
			if ((!n->second->flags_needed) || (user->modes[n->second->flags_needed-65]))
			{
				if (n->second->flags_needed)
				{
					return ((user->HasPermission(commandname)) || (is_uline(user->server)));
				}
				return true;
			}
		}
	}
	return false;
}

// calls a handler function for a command

bool CommandParser::CallHandler(const std::string &commandname,const char** parameters, int pcnt, userrec *user)
{
	nspace::hash_map<std::string,command_t*>::iterator n = cmdlist.find(commandname);

	if (n != cmdlist.end())
	{
		if (pcnt >= n->second->min_params)
		{
			if ((!n->second->flags_needed) || (user->modes[n->second->flags_needed-65]))
			{
				if (n->second->flags_needed)
				{
					if ((user->HasPermission(commandname)) || (!IS_LOCAL(user)))
					{
						n->second->Handle(parameters,pcnt,user);
						return true;
					}
				}
				else
				{
					n->second->Handle(parameters,pcnt,user);
					return true;
				}
			}
		}
	}
	return false;
}

void CommandParser::ProcessCommand(userrec *user, std::string &cmd)
{
	const char *command_p[127];
	int items = 0;
	std::string para[127];
	irc::tokenstream tokens(cmd);
	std::string command = tokens.GetToken();

	while (((para[items] = tokens.GetToken()) != "") && (items < 127))
		command_p[items] = para[items++].c_str();

	for (std::string::iterator makeupper = command.begin(); makeupper != command.end(); makeupper++)
		*makeupper = toupper(*makeupper);
		
	int MOD_RESULT = 0;
	FOREACH_RESULT(I_OnPreCommand,OnPreCommand(command,command_p,items,user,false));
	if (MOD_RESULT == 1) {
		return;
	}

	nspace::hash_map<std::string,command_t*>::iterator cm = cmdlist.find(command);
	
	if (cm != cmdlist.end())
	{
		if (user)
		{
			/* activity resets the ping pending timer */
			user->nping = TIME + user->pingmax;
			if (items < cm->second->min_params)
			{
				log(DEBUG,"not enough parameters: %s %s",user->nick,command.c_str());
				WriteServ(user->fd,"461 %s %s :Not enough parameters",user->nick,command.c_str());
				return;
			}
			if (cm->second->flags_needed)
			{
				if (!user->IsModeSet(cm->second->flags_needed))
				{
					log(DEBUG,"permission denied: %s %s",user->nick,command.c_str());
					WriteServ(user->fd,"481 %s :Permission Denied- You do not have the required operator privilages",user->nick);
					return;
				}
			}
			if ((cm->second->flags_needed) && (!user->HasPermission(command)))
			{
				log(DEBUG,"permission denied: %s %s",user->nick,command.c_str());
				WriteServ(user->fd,"481 %s :Permission Denied- Oper type %s does not have access to command %s",user->nick,user->oper,command.c_str());
				return;
			}
			if ((user->registered == 7) && (!*user->oper) && (cm->second->IsDisabled()))
			{
				/* command is disabled! */
				WriteServ(user->fd,"421 %s %s :This command has been disabled.",user->nick,command.c_str());
				return;
			}
			if ((user->registered == 7) || (cm->second == command_user) || (cm->second == command_nick) || (cm->second == command_pass))
			{
				/* ikky /stats counters */
				cm->second->use_count++;
				cm->second->total_bytes += cmd.length();

				int MOD_RESULT = 0;
				FOREACH_RESULT(I_OnPreCommand,OnPreCommand(command,command_p,items,user,true));
				if (MOD_RESULT == 1)
					return;

				/*
				 * WARNING: nothing may come after the
				 * command handler call, as the handler
				 * may free the user structure!
				 */

				cm->second->Handle(command_p,items,user);
				return;
			}
			else
			{
				WriteServ(user->fd,"451 %s :You have not registered",command.c_str());
				return;
			}
		}
	}
	else if (user)
	{
		ServerInstance->stats->statsUnknown++;
		WriteServ(user->fd,"421 %s %s :Unknown command",user->nick,command.c_str());
	}
}

bool CommandParser::RemoveCommands(const char* source)
{
	bool go_again = true;

	while (go_again)
	{
		go_again = false;

		for (nspace::hash_map<std::string,command_t*>::iterator i = cmdlist.begin(); i != cmdlist.end(); i++)
		{
			command_t* x = i->second;
			if (x->source == std::string(source))
			{
				log(DEBUG,"removecommands(%s) Removing dependent command: %s",x->source.c_str(),x->command.c_str());
				cmdlist.erase(i);
				go_again = true;
				break;
			}
		}
	}

	return true;
}

void CommandParser::ProcessBuffer(std::string &buffer,userrec *user)
{
	std::string::size_type a;

	if (!user)
		return;

	while ((a = buffer.find("\n")) != std::string::npos)
		buffer.erase(a);
	while ((a = buffer.find("\r")) != std::string::npos)
		buffer.erase(a);

	log(DEBUG,"CMDIN: %s %s",user->nick,buffer.c_str());

	this->ProcessCommand(user,buffer);
}

bool CommandParser::CreateCommand(command_t *f)
{
	/* create the command and push it onto the table */
	if (cmdlist.find(f->command) == cmdlist.end())
	{
		cmdlist[f->command] = f;
		log(DEBUG,"Added command %s (%lu parameters)",f->command.c_str(),(unsigned long)f->min_params);
		return true;
	}
	else return false;
}

CommandParser::CommandParser()
{
	this->SetupCommandTable();
}

void CommandParser::SetupCommandTable()
{
	/* These three are special (can occur without
	 * full user registration) and so are saved
	 * for later use.
	 */
	command_user = new cmd_user;
	command_nick = new cmd_nick;
	command_pass = new cmd_pass;
	this->CreateCommand(command_user);
	this->CreateCommand(command_nick);
	this->CreateCommand(command_pass);

	/* The rest of these arent special. boo hoo.
	 */
	this->CreateCommand(new cmd_quit);
	this->CreateCommand(new cmd_version);
	this->CreateCommand(new cmd_ping);
	this->CreateCommand(new cmd_pong);
	this->CreateCommand(new cmd_admin);
	this->CreateCommand(new cmd_privmsg);
	this->CreateCommand(new cmd_info);
	this->CreateCommand(new cmd_time);
	this->CreateCommand(new cmd_whois);
	this->CreateCommand(new cmd_wallops);
	this->CreateCommand(new cmd_notice);
	this->CreateCommand(new cmd_join);
	this->CreateCommand(new cmd_names);
	this->CreateCommand(new cmd_part);
	this->CreateCommand(new cmd_kick);
	this->CreateCommand(new cmd_mode);
	this->CreateCommand(new cmd_topic);
	this->CreateCommand(new cmd_who);
	this->CreateCommand(new cmd_motd);
	this->CreateCommand(new cmd_rules);
	this->CreateCommand(new cmd_oper);
	this->CreateCommand(new cmd_list);
	this->CreateCommand(new cmd_die);
	this->CreateCommand(new cmd_restart);
	this->CreateCommand(new cmd_kill);
	this->CreateCommand(new cmd_rehash);
	this->CreateCommand(new cmd_lusers);
	this->CreateCommand(new cmd_stats);
	this->CreateCommand(new cmd_userhost);
	this->CreateCommand(new cmd_away);
	this->CreateCommand(new cmd_ison);
	this->CreateCommand(new cmd_summon);
	this->CreateCommand(new cmd_users);
	this->CreateCommand(new cmd_invite);
	this->CreateCommand(new cmd_trace);
	this->CreateCommand(new cmd_whowas);
	this->CreateCommand(new cmd_connect);
	this->CreateCommand(new cmd_squit);
	this->CreateCommand(new cmd_modules);
	this->CreateCommand(new cmd_links);
	this->CreateCommand(new cmd_map);
	this->CreateCommand(new cmd_kline);
	this->CreateCommand(new cmd_gline);
	this->CreateCommand(new cmd_zline);
	this->CreateCommand(new cmd_qline);
	this->CreateCommand(new cmd_eline);
	this->CreateCommand(new cmd_loadmodule);
	this->CreateCommand(new cmd_unloadmodule);
	this->CreateCommand(new cmd_server);
	this->CreateCommand(new cmd_commands);
}
