#include <znc/main.h>
#include <znc/User.h>
#include <znc/IRCNetwork.h>
#include <znc/Modules.h>
#include <znc/Nick.h>
#include <znc/Chan.h>
#include <znc/IRCSock.h>
#include <znc/version.h>

#include <mutex>
#include <limits>

#include "twitchtmi.h"
#include "jload.h"

#if (VERSION_MAJOR < 1) || (VERSION_MAJOR == 1 && VERSION_MINOR < 7)
#error This module needs at least ZNC 1.7.0 or later.
#endif

TwitchTMI::~TwitchTMI()
{

}

bool TwitchTMI::OnLoad(const CString& sArgsi, CString& sMessage)
{
	OnBoot();

	if(GetNetwork())
	{
		std::list<CString> channels;

		for(CChan *ch: GetNetwork()->GetChans())
		{
			CString sname = ch->GetName().substr(1);
			channels.push_back(sname);

			ch->SetTopic(GetNV("tmi_topic_" + sname));
			ch->SetTopicOwner("jtv");
			ch->SetTopicDate(GetNV("tmi_topic_time_" + sname).ToULong());
		}

		CThreadPool::Get().addJob(new TwitchTMIJob(this, channels));
	}

	if(GetArgs().Token(0) != "FrankerZ")
		lastFrankerZ = lastPlay = std::numeric_limits<decltype(lastFrankerZ)>::max();

	PutIRC("CAP REQ :twitch.tv/membership");
	PutIRC("CAP REQ :twitch.tv/commands");
	PutIRC("CAP REQ :twitch.tv/tags");

	return true;
}

bool TwitchTMI::OnBoot()
{
	initCurl();

	timer = new TwitchTMIUpdateTimer(this);
	AddTimer(timer);

	return true;
}

void TwitchTMI::OnIRCConnected()
{
	PutIRC("CAP REQ :twitch.tv/membership");
	PutIRC("CAP REQ :twitch.tv/commands");
	PutIRC("CAP REQ :twitch.tv/tags");
}

CModule::EModRet TwitchTMI::OnUserRawMessage(CMessage &msg)
{
	const CString &cmd = msg.GetCommand();
	if(cmd.Equals("WHO") || cmd.Equals("AWAY") || cmd.Equals("TWITCHCLIENT") || cmd.Equals("JTVCLIENT"))
	{
		return CModule::HALT;
	}
	else if(cmd.Equals("KICK"))
	{
		VCString params = msg.GetParams();
		if(params.size() < 2)
			return CModule::HALT;
		msg.SetCommand("PRIVMSG");
		params = { params[0], "/timeout " + CString(" ").Join(std::next(params.begin()), params.end()) };
		msg.SetParams(params);
	}
	else if(cmd.Equals("MODE"))
	{
		VCString params = msg.GetParams();
		if(params.size() != 3)
			return CModule::HALT;

		CString victim, mode = params[1];

		CNick victimNick;
		victimNick.Parse(params[2]);

		if(mode.Equals("+b"))
			mode = "/ban";
		else if(mode.Equals("-b"))
			mode = "/unban";
		else if(mode.Equals("+o"))
			mode = "/mod";
		else if(mode.Equals("-o"))
			mode = "/unmod";
		else
		{
			PutStatus("Unknown mode change: " + mode);
			return CModule::HALT;
		}

		if(victimNick.GetHost().Contains(".tmi.twitch.tv"))
			victim = victimNick.GetHost().Token(0, false, ".");
		else
			victim = victimNick.GetNick();

		msg.SetCommand("PRIVMSG");
		params = { params[0], mode + " " + victim };
		msg.SetParams(params);
	}

	return CModule::CONTINUE;
}

static CString subPlanToName(const CString &plan)
{
	if(plan.AsLower() == "prime")
		return "Twitch Prime";
	else if(plan == "1000")
		return "Tier 1";
	else if(plan == "2000")
		return "Tier 2";
	else if(plan == "3000");
		return "Tier 3";
	return plan;
}

CModule::EModRet TwitchTMI::OnRawMessage(CMessage &msg)
{
	CString realNick = msg.GetTag("display-name").Trim_n();
	if (realNick == "")
		realNick = msg.GetNick().GetNick();

	if(msg.GetCommand().Equals("HOSTTARGET"))
	{
		return CModule::HALT;
	}
	else if(msg.GetCommand().Equals("CLEARCHAT"))
	{
		msg.SetCommand("NOTICE");
		if(msg.GetParam(1) != "")
		{
			msg.SetParam(1, msg.GetParam(1) + " was timed out.");
		}
		else
		{
			msg.SetParam(1, "Chat was cleared by a moderator.");
		}
	}
	else if(msg.GetCommand().Equals("USERSTATE"))
	{
		return CModule::HALT;
	}
	else if(msg.GetCommand().Equals("ROOMSTATE"))
	{
		return CModule::HALT;
	}
	else if(msg.GetCommand().Equals("RECONNECT"))
	{
		return CModule::HALT;
	}
	else if(msg.GetCommand().Equals("GLOBALUSERSTATE"))
	{
		return CModule::HALT;
	}
	else if(msg.GetCommand().Equals("WHISPER"))
	{
		msg.SetCommand("PRIVMSG");
	}
	else if(msg.GetCommand().Equals("USERNOTICE"))
	{
		CString msg_id = msg.GetTag("msg-id");
		CString new_msg = "<unknown usernotice " + msg_id + "> from " + realNick;
		if(msg_id == "sub" || msg_id == "resub") {
			CString dur = msg.GetTag("msg-param-months");
			CString plan = subPlanToName(msg.GetTag("msg-param-sub-plan"));
			CString txt = msg.GetParam(1).Trim_n();

			new_msg = realNick + " just ";
			if(msg_id == "sub")
				new_msg += "subscribed with a " + plan + " sub";
			else
				new_msg += "resubscribed with a " + plan + " sub for " + dur + " months";

			if(txt != "")
				new_msg += " saying: " + txt;
			else
				new_msg += "!";

			realNick = "jtv";
		} else if(msg_id == "subgift") {
			CString rec = msg.GetTag("msg-param-recipient-display-name");
			CString plan = subPlanToName(msg.GetTag("msg-param-sub-plan"));

			new_msg = realNick + " gifted a " + plan + " sub to " + rec + "!";
		} else if(msg_id == "raid") {
			CString cnt = msg.GetTag("msg-param-viewerCount");
			CString src = msg.GetTag("msg-param-displayName");

			new_msg = src + " is raiding with a party of " + cnt + "!";
		} else if(msg_id == "ritual") {
			CString rit = msg.GetTag("msg-param-ritual-name");
			CString txt = msg.GetParam(1).Trim_n();

			new_msg = "<unknown ritual " + rit + ">";
			if(rit == "new_chatter")
				new_msg = realNick + " is new here, saying: " + txt;
		} else if(msg_id == "giftpaidupgrade") {
			new_msg = msg.GetTag("system-msg");
		}

		realNick = "ttv";
		new_msg = ">>> " + new_msg + " <<<";
		msg.SetCommand("PRIVMSG");
		msg.SetParam(1, new_msg);
	}

	msg.GetNick().SetNick(realNick);

	if(realNick == "ttv")
		msg.GetNick().Parse("ttv!ttv@ttv.tmi.twitch.tv");

	return CModule::CONTINUE;
}

CModule::EModRet TwitchTMI::OnUserJoin(CString& sChannel, CString& sKey)
{
	return CModule::CONTINUE;
}

CModule::EModRet TwitchTMI::OnPrivTextMessage(CTextMessage &Message)
{
	if(Message.GetNick().GetNick().Equals("jtv"))
		return CModule::HALT;

	return CModule::CONTINUE;
}

void TwitchTMI::PutUserChanMessage(CChan *chan, const CString &from, const CString &msg)
{
	std::stringstream ss;
	ss << ":" << from << " PRIVMSG " << chan->GetName() << " :";
	CString s = ss.str();

	PutUser(s + msg);

	if(!chan->AutoClearChanBuffer() || !GetNetwork()->IsUserOnline() || chan->IsDetached())
		chan->AddBuffer(s + "{text}", msg);
}

void TwitchTMI::InjectMessageHelper(CChan *chan, const CString &action)
{
	std::stringstream ss;
	CString mynick = GetNetwork()->GetIRCNick().GetNickMask();

	ss << "PRIVMSG " << chan->GetName() << " :" << action;
	PutIRC(ss.str());

	AddJob(new GenericJob(this, "put_msg_job_" + chan->GetName(), "put msg helper", []() {}, [this, chan, mynick, action]()
	{
		PutUserChanMessage(chan, mynick, action);
	}));
}

CModule::EModRet TwitchTMI::OnChanTextMessage(CTextMessage &Message)
{
	if(Message.GetNick().GetNick().Equals("jtv"))
		return CModule::HALT;

	if(Message.GetNick().GetNick().AsLower().Equals("hentaitheace"))
		return CModule::CONTINUE;

	CChan *chan = Message.GetChan();

	if(Message.GetText() == "FrankerZ" && std::time(nullptr) - lastFrankerZ > 10)
	{
		InjectMessageHelper(chan, "FrankerZ");
		lastFrankerZ = std::time(nullptr);
	}
	else if(Message.GetText() == "!play" && std::time(nullptr) - lastPlay > 135)
	{
		AddTimer(new GenericTimer(this, 15, 1, "play_timer_" + chan->GetName(), "Writes !play in 15 sec!", [this, chan]()
		{
			InjectMessageHelper(chan, "!play");
		}));
		lastPlay = std::time(nullptr);
	}

	return CModule::CONTINUE;
}

bool TwitchTMI::OnServerCapAvailable(const CString &sCap)
{
	if(sCap == "twitch.tv/membership")
		return true;
	else if(sCap == "twitch.tv/tags")
		return true;
	else if(sCap == "twitch.tv/commands")
		return true;

	return false;
}

CModule::EModRet TwitchTMI::OnUserTextMessage(CTextMessage &msg)
{
	if(msg.GetTarget().Left(1).Equals("#"))
		return CModule::CONTINUE;

	msg.SetText(msg.GetText().insert(0, " ").insert(0, msg.GetTarget()).insert(0, "/w "));
	msg.SetTarget("#jtv");

	return CModule::CONTINUE;
}


TwitchTMIUpdateTimer::TwitchTMIUpdateTimer(TwitchTMI *tmod)
	:CTimer(tmod, 30, 0, "TwitchTMIUpdateTimer", "Downloads Twitch information")
	,mod(tmod)
{
}

void TwitchTMIUpdateTimer::RunJob()
{
	if(!mod->GetNetwork())
		return;

	std::list<CString> channels;

	for(CChan *chan: mod->GetNetwork()->GetChans())
	{
		channels.push_back(chan->GetName().substr(1));
	}

	mod->AddJob(new TwitchTMIJob(mod, channels));
}

static std::mutex job_thread_lock;

void TwitchTMIJob::runThread()
{
	std::unique_lock<std::mutex> lock_guard(job_thread_lock, std::try_to_lock);

	if(!lock_guard.owns_lock())
	{
		channels.clear();
		return;
	}

	titles.resize(channels.size());
	lives.resize(channels.size());

	int i = -1;
	for(const CString &channel: channels)
	{
		i++;

		std::stringstream ss, ss2;
		ss << "https://api.twitch.tv/kraken/channels/" << channel;
		ss2 << "https://api.twitch.tv/kraken/streams/" << channel;

		CString url = ss.str();
		CString url2 = ss2.str();

		Json::Value root = getJsonFromUrl(url.c_str(), "Accept: application/vnd.twitchtv.v3+json");
		Json::Value root2 = getJsonFromUrl(url2.c_str(), "Accept: application/vnd.twitchtv.v3+json");

		if(!root.isNull())
		{
			Json::Value &titleVal = root["status"];
			titles[i] = CString();

			if(!titleVal.isString())
				titleVal = root["title"];

			if(titleVal.isString())
			{
				titles[i] = titleVal.asString();
				titles[i].Trim();
			}
		}

		lives[i] = false;

		if(!root2.isNull())
		{
			Json::Value &streamVal = root2["stream"];

			if(!streamVal.isNull())
				lives[i] = true;
		}
    }
}

void TwitchTMIJob::runMain()
{
	int i = -1;
	for(const CString &channel: channels)
	{
		i++;

		CChan *chan = mod->GetNetwork()->FindChan(CString("#") + channel);
		if(!chan)
			continue;

		if(!titles[i].empty() && chan->GetTopic() != titles[i])
		{
			unsigned long topic_time = (unsigned long)time(nullptr);
			chan->SetTopic(titles[i]);
			chan->SetTopicOwner("jtv");
			chan->SetTopicDate(topic_time);

			std::stringstream ss;
			ss << ":jtv TOPIC #" << channel << " :" << titles[i];

			mod->PutUser(ss.str());

			mod->SetNV("tmi_topic_" + channel, titles[i], true);
			mod->SetNV("tmi_topic_time_" + channel, CString(topic_time), true);
		}

		auto it = mod->liveChannels.find(channel);
		if(it != mod->liveChannels.end())
		{
			if(!lives[i])
			{
				mod->liveChannels.erase(it);
				mod->PutUserChanMessage(chan, "jtv", ">>> Channel just went offline! <<<");
			}
		}
		else
		{
			if(lives[i])
			{
				mod->liveChannels.insert(channel);
				mod->PutUserChanMessage(chan, "jtv", ">>> Channel just went live! <<<");
			}
		}
	}
}


template<> void TModInfo<TwitchTMI>(CModInfo &info)
{
	info.SetWikiPage("Twitch");
	info.SetHasArgs(true);
}

NETWORKMODULEDEFS(TwitchTMI, "Twitch IRC helper module")
