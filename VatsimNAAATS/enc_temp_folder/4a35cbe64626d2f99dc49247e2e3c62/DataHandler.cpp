#include "pch.h"
#include "DataHandler.h"
#include "RoutesHelper.h"
#include <iostream>
#include <fstream>
#include <json.hpp>
#include "ApiSettings.h"
#include <WinInet.h>
#include "curl/curl.h"
#pragma comment(lib,"WinInet.Lib")

// Include dependency
using json = nlohmann::json;

const string CDataHandler::TrackURL = "https://tracks.ganderoceanic.com/data";
const string CDataHandler::EventTrackUrl = "https://tracks.ganderoceanic.com/event";
map<string, CAircraftFlightPlan> CDataHandler::flights;

int CDataHandler::PopulateLatestTrackData(CPlugIn* plugin) {
	// Try and get data and pass into string
	string responseString;
	try {
		// Convert URL to LPCSTR type
		LPCSTR lpcURL = TrackURL.c_str();

		// Delete cache data
		DeleteUrlCacheEntry(lpcURL);

		// Download data
		CComPtr<IStream> pStream;
		HRESULT hr = URLOpenBlockingStream(NULL, lpcURL, &pStream, 0, NULL);
		// If failed
		if (FAILED(hr)) {
			// Show user message
			plugin->DisplayUserMessage("vNAAATS", "Error", "Failed to load NAT Track data.", true, true, true, true, true);
			return 1;
		}
		// Put data into buffer
		char tempBuffer[2048];
		DWORD bytesRead = 0;
		hr = pStream->Read(tempBuffer, sizeof(tempBuffer), &bytesRead);
		// Put data into string
		for (int i = 0; i < bytesRead; i++) {
			responseString += tempBuffer[i];
		}
	}
	catch (exception & e) {
		plugin->DisplayUserMessage("vNAAATS", "Error", string("Failed to load NAT Track data: " + string(e.what())).c_str(), true, true, true, true, true);
		return 1;
	}
	
	try {
		// Clear old tracks
		if (!CRoutesHelper::CurrentTracks.empty()) {
			CRoutesHelper::CurrentTracks.clear();
		}
		
		// Now we parse the json
		auto jsonArray = json::parse(responseString);
		for (int i = 0; i < jsonArray.size(); i++) {
			// Make track
			CTrack track;

			// Identifier
			track.Identifier = jsonArray[i].at("id");

			// TMI
			track.TMI = jsonArray[i].at("tmi");
			CRoutesHelper::CurrentTMI = jsonArray[i].at("tmi");

			// Direction
			if (jsonArray[i].at("direction") == 0) {
				track.Direction = CTrackDirection::UNKNOWN;
			}
			else if (jsonArray[i].at("direction") == 1) {
				track.Direction = CTrackDirection::WEST;
			}
			else {
				track.Direction = CTrackDirection::EAST;
			}

			// Route
			for (int j = 0; j < jsonArray[i].at("route").size(); j++) {
				track.Route.push_back(jsonArray[i].at("route")[j].at("name"));
				track.RouteRaw.push_back(CUtils::PositionFromLatLon(jsonArray[i].at("route")[j].at("latitude"), jsonArray[i].at("route")[j].at("longitude")));
			}

			// Flight levels
			for (int j = 0; j < jsonArray[i].at("flightLevels").size(); j++) {
				track.FlightLevels.push_back((int)jsonArray[i].at("flightLevels")[j]);
			}

			// Push track to tracks array
			CRoutesHelper::CurrentTracks.insert(make_pair(track.Identifier, track));
		}
		// Everything succeeded, show to user
		plugin->DisplayUserMessage("Message", "vNAAATS Plugin", string("Track data loaded successfully. TMI is " + CRoutesHelper::CurrentTMI + ".").c_str(), false, false, false, false, false);
		return 0;
	}
	catch (exception & e) {
		plugin->DisplayUserMessage("vNAAATS", "Error", string("Failed to parse NAT track data: " + string(e.what())).c_str(), true, true, true, true, true);
		return 1;
	}
}

CAircraftFlightPlan* CDataHandler::GetFlightData(string callsign) {
	if (flights.find(callsign) != flights.end()) {
		// Ben: this is for you, I have done the 'not found' code at the bottom you need to do the code that gets the existing flight data
		// not sure whether it's as simple as return flights.at(callsign) or something more complex, depends on the API integration
		
		return &flights.find(callsign)->second;
	}
	// Return invalid
	CAircraftFlightPlan fp;
	fp.IsValid = false;
	return &fp;
}

int CDataHandler::UpdateFlightData(CRadarScreen* screen, string callsign, bool updateRoute) {
	// Flight plan
	auto fp = flights.find(callsign);

	// If flight plan not found return non-success code
	if (fp == flights.end()) {
		return 1;
	}

	// Ben: I have done the update route part for the initialisation if passed in, but you need to do the 
	// update querying to update the flight data, maybe make an IsFlightDataUpdated() method returning the
	// updated data from natTrak if it has been updated and returning an FP object with IsValid == false if not updated

	// Re-initialise the route if requested
	if (updateRoute) {
		CUtils::CAsyncData data;
		data.Screen = screen;
		data.Callsign = callsign;
		_beginthread(CRoutesHelper::InitialiseRoute, 0, (void*) &data); // Async
	}

	// Success
	return 0;
}

int CDataHandler::CreateFlightData(CRadarScreen* screen, string callsign) {
	// MISSING VALUES:
	// DLStatus
	// FLIGHT LEVEL
	// MACH
	// SECTOR
	// STATE

	// Euroscope flight plan and my flight plan
	CFlightPlan fpData = screen->GetPlugIn()->FlightPlanSelect(callsign.c_str());
	CAircraftFlightPlan fp;	
	// Instantiate aircraft information
	fp.Callsign = callsign;
	fp.Type = fpData.GetFlightPlanData().GetAircraftFPType();
	fp.Depart = fpData.GetFlightPlanData().GetOrigin();
	fp.Dest = fpData.GetFlightPlanData().GetDestination();
	fp.Etd = CUtils::ParseZuluTime(false, atoi(fpData.GetFlightPlanData().GetEstimatedDepartureTime()));
	fp.ExitTime = screen->GetPlugIn()->FlightPlanSelect(callsign.c_str()).GetSectorExitMinutes();
	fp.DLStatus = ""; // TEMPORARY
	fp.Sector = string(fpData.GetTrackingControllerId()) == "" ? "-1" : fpData.GetTrackingControllerId();
	fp.CurrentMessage = nullptr;

	// Get SELCAL code
	string remarks = fpData.GetFlightPlanData().GetRemarks();
	size_t found = remarks.find(string("SEL/"));
	// If found
	if (found != string::npos) {
		fp.SELCAL = remarks.substr(found + 4, 4);
	}
	else {
		fp.SELCAL= "";
	}

	// Get communication mode
	string comType;
	comType += toupper(fpData.GetFlightPlanData().GetCommunicationType());
	if (comType == "V") { // Switch each of the values
		comType = "VOX";
	}
	else if (comType == "T") {
		comType = "TXT";
	}
	else if (comType == "R") {
		comType = "RCV";
	}
	else {
		comType = "VOX"; // We assume voice if it defaults
	}
	fp.Communications = comType; // Set the type

	// TODO: PULL NATTRAK DATA AND FILL RESTRICTIONS, CURRENTMESSAGE AND FLIGHTHISTORY, ALSO ROUTERAW IF IT IS THERE, SAME WITH ISCLEARED

	// Set IsCleared (TEMPORARY)
	fp.IsCleared = false;

	// Flight plan is valid
	fp.IsValid = true;

	// Add FP to map
	flights[callsign] = fp;

	// Generate route
	CUtils::CAsyncData* data = new CUtils::CAsyncData();
	data->Screen = screen;
	data->Callsign = callsign;
	_beginthread(CRoutesHelper::InitialiseRoute, 0, (void*) data); // Async

	// Success
	return 0;
}

int CDataHandler::DeleteFlightData(string callsign) {
	if (flights.find(callsign) != flights.end()) {
		// Remove the flight if it exists
		flights.erase(callsign);
		return 0;
	}
	else {
		return 1; // Non-success code occurs when flight doesn't exist to delete
	}
}

int CDataHandler::SetRoute(string callsign, vector<CWaypoint>* route, string track) {
	if (flights.find(callsign) != flights.end()) {
		// Set route if flight exists
		flights.find(callsign)->second.Route.clear();
		flights.find(callsign)->second.Route = *route;

		// Set track if not nothing
		if (track != "")
			flights.find(callsign)->second.Track = track;
		else 
			flights.find(callsign)->second.Track = "RR";

		// Success code
		return 0;
	}
	else {
		return 1; // Non-success code occurs when flight doesn't exist
	}
}

size_t CDataHandler::WriteApiCallback(void* contents, size_t size, size_t nmemb, void* userp)
{
	((std::string*)userp)->append((char*)contents, size * nmemb);
	return size * nmemb;
}

CAircraftFlightPlan* CDataHandler::ApiGetFlightData(string callsign)
{
	string result;
	CURL* curl;
	CURLcode result_code;

	curl_global_init(CURL_GLOBAL_DEFAULT);

	curl = curl_easy_init();
	if (curl)
	{
		stringstream url;
		url << ApiSettings::apiUrl << "/flight_data/get.php?apiKey=" << ApiSettings::apiKey << "&callsign=" << callsign;
		curl_easy_setopt(curl, CURLOPT_URL, url.str().c_str());
		curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CDataHandler::WriteApiCallback);
		curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);
		curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

		result_code = curl_easy_perform(curl);
		
		curl_easy_cleanup(curl);

		if (CURLE_OK != result_code && result.length() <= 0)
		{
			auto* flight_plan = new CAircraftFlightPlan;
			return flight_plan;
		}
		else
		{
			auto const presult = nlohmann::json::parse(result);
			if(presult.size() > 1) //if >1 row was returned (shouldn't be any dupes!)
			{
				auto* flight_plan = new CAircraftFlightPlan;
				return flight_plan;
			}

			auto const row = presult[0];

			string route,
				   callsign,
				   destination,
			       logged_onto,
				   assigned_level,
			       assigned_mach,
			       track;
			
			//null checking
			if(!row.at("route").is_null())
			{
				route = (string)row.at("route");
			} else
			{
				route = nullptr;
			}

			if (!row.at("callsign").is_null())
			{
				callsign = (string)row.at("callsign");
			} else
			{
				callsign = nullptr;
			}
			
			if (!row.at("destination").is_null())
			{
				destination = (string)row.at("destination");
			} else
			{
				destination = nullptr;
			}
			
			if (!row.at("logged_onto").is_null())
			{
				logged_onto = (string)row.at("logged_onto");
			} else
			{
				logged_onto = nullptr;
			}
			
			if (!row.at("assigned_level").is_null())
			{
				assigned_level = (string)row.at("assigned_level");
			} else
			{
				assigned_level = nullptr;
			}
			
			if (!row.at("assigned_mach").is_null())
			{
				assigned_mach = (string)row.at("assigned_mach");
			} else
			{
				assigned_mach = nullptr;
			}
			
			if (!row.at("track").is_null())
			{
				track = (string)row.at("track");
			} else
			{
				track = nullptr;
			}

			//end null checking
			
			auto* flight_plan = new CAircraftFlightPlan(
				route,
				callsign,
				nullptr,
				nullptr,
				destination,
				nullptr,
				nullptr,
				nullptr,
				nullptr,
				logged_onto,
				assigned_level,
				assigned_mach,
				track,
				nullptr,
				0,
				false,
				true 
			);

			bool is_track = false;
			if((string)row.at("track") != "RR")
			{
				is_track = true;
			}

			CRoutesHelper::ParseRoute((string)row.at("callsign"), (string)row.at("route"), is_track);
			
			return flight_plan;
		}
	}
}