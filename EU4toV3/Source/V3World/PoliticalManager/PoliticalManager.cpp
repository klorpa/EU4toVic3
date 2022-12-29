#include "PoliticalManager.h"
#include "ClayManager/ClayManager.h"
#include "ClayManager/State/State.h"
#include "ClayManager/State/SubState.h"
#include "Country/Country.h"
#include "CountryManager/EU4Country.h"
#include "DiplomacyParser/EU4Agreement.h"
#include "Loaders/CountryDefinitionLoader/CountryDefinitionLoader.h"
#include "Loaders/LawLoader/LawLoader.h"
#include "Log.h"
#include "Mappers/CountryMapper/CountryMapper.h"
#include "PopManager/PopManager.h"
#include "TechValues/TechValues.h"
#include <cmath>
#include <ranges>

void V3::PoliticalManager::initializeVanillaCountries(const commonItems::ModFilesystem& modFS)
{
	Log(LogLevel::Info) << "-> Loading Vanilla Countries.";

	CountryDefinitionLoader definitionLoader;
	definitionLoader.loadCommonCountries(modFS);
	countries = definitionLoader.getCountries();

	Log(LogLevel::Info) << "<> " << countries.size() << " vanilla countries loaded.";
}

void V3::PoliticalManager::loadCountryMapper(const std::shared_ptr<mappers::CountryMapper>& theCountryMapper)
{
	countryMapper = theCountryMapper;
	for (const auto& countryTag: countries | std::views::keys)
		countryMapper->registerKnownVanillaV3Tag(countryTag);
}

void V3::PoliticalManager::loadPopulationSetupMapperRules(const std::string& filePath)
{
	populationSetupMapper.loadMappingRules(filePath);
}

void V3::PoliticalManager::loadIdeaEffectMapperRules(const std::string& filePath)
{
	ideaEffectMapper.loadMappingRules(filePath);
}

void V3::PoliticalManager::loadTechSetupMapperRules(const std::string& filePath)
{
	techSetupMapper.loadMappingRules(filePath);
}

void V3::PoliticalManager::loadLawMapperRules(const std::string& filePath)
{
	lawMapper.loadMappingRules(filePath);
}

void V3::PoliticalManager::loadLawDefinitions(const commonItems::ModFilesystem& modFS)
{
	LawLoader loader;
	loader.loadLaws(modFS);
	lawMapper.loadLawDefinitions(loader.getLaws());
}

void V3::PoliticalManager::loadDiplomaticMapperRules(const std::string& filePath)
{
	diplomaticMapper.loadMappingRules(filePath);
}

void V3::PoliticalManager::importEU4Countries(const std::map<std::string, std::shared_ptr<EU4::Country>>& eu4Countries)
{
	Log(LogLevel::Info) << "-> Moving over EU4 Countries.";

	for (const auto& eu4country: eu4Countries | std::views::values)
	{
		auto v3Tag = countryMapper->assignV3TagToEU4Country(eu4country);

		std::shared_ptr<Country> country;
		if (countries.contains(v3Tag))
		{
			country = countries.at(v3Tag);
		}
		else
		{
			country = std::make_shared<Country>();
			country->setTag(v3Tag);
			countries.emplace(v3Tag, country);
		}

		country->setSourceCountry(eu4country);
	}

	Log(LogLevel::Info) << "<> Vic3 now has " << countries.size() << " known countries.";
}

void V3::PoliticalManager::generateDecentralizedCountries(const ClayManager& clayManager, const PopManager& popManager)
{
	Log(LogLevel::Info) << "-> Generating decentralized countries.";

	// How do we generate decentralized states? We look at states and their unowned substates.
	// Those substates with a particular culture are banded together and shoved into a new country.
	// If there's no demographic available (mapping from a wasteland or out of scope), we load
	// cultural information (not demographics!) from vanilla sources.

	auto sortedSubstates = sortSubStatesByCultures(clayManager, popManager);

	for (const auto& [culture, subStates]: sortedSubstates)
	{
		generateDecentralizedCountry(culture, subStates);
	}

	Log(LogLevel::Info) << "<> Generated " << sortedSubstates.size() << " decentralized countries.";
}

void V3::PoliticalManager::generateDecentralizedCountry(const std::string& culture, const std::vector<std::shared_ptr<SubState>>& subStates)
{
	auto v3tag = countryMapper->requestNewV3Tag();
	auto newCountry = std::make_shared<Country>();
	newCountry->setTag(v3tag);
	newCountry->setSubStates(subStates);
	ProcessedData data;
	data.cultures.emplace(culture);
	data.type = "decentralized";
	newCountry->setProcessedData(data);
	for (const auto& subState: newCountry->getSubStates())
		subState->setOwner(newCountry);

	countries.emplace(v3tag, newCountry);
	// ... fingers crossed?
}

V3::PoliticalManager::CulturalSubStates V3::PoliticalManager::sortSubStatesByCultures(const ClayManager& clayManager, const PopManager& popManager)
{
	CulturalSubStates culturalSubStates;

	for (const auto& [stateName, state]: clayManager.getStates())
	{
		for (const auto& subState: state->getSubStates())
		{
			if (subState->getOwner()) // don't need owned substates.
				continue;

			// Does this substate have a leading demographic? Or any demographic?
			std::string leadCulture;
			if (!subState->getDemographics().empty())
			{
				leadCulture = getDominantDemographic(subState->getDemographics());
			}
			else
			{
				// Ask popmanager for something vanillaish for the entire state (can't use our substates for this).
				leadCulture = popManager.getDominantVanillaCulture(stateName);
			}
			if (!culturalSubStates.contains(leadCulture))
				culturalSubStates.emplace(leadCulture, std::vector<std::shared_ptr<SubState>>{});
			culturalSubStates.at(leadCulture).push_back(subState);
		}
	}

	return culturalSubStates;
}

std::string V3::PoliticalManager::getDominantDemographic(const std::vector<Demographic>& demographics)
{
	std::map<std::string, double> cultureScores;

	for (const auto& demo: demographics)
	{
		if (cultureScores.contains(demo.culture))
			cultureScores.at(demo.culture) += demo.lowerRatio; // using lower ratio to represent farmers - the majority.
		else
			cultureScores.emplace(demo.culture, demo.lowerRatio);
	}

	const auto highest = std::max_element(std::begin(cultureScores), std::end(cultureScores), [](const auto& p1, const auto& p2) {
		return p1.second < p2.second;
	});

	return highest->first;
}

void V3::PoliticalManager::convertAllCountries(const ClayManager& clayManager,
	 mappers::CultureMapper& cultureMapper,
	 const mappers::ReligionMapper& religionMapper,
	 const EU4::CultureLoader& cultureLoader,
	 const EU4::ReligionLoader& religionLoader,
	 const LocalizationLoader& v3LocLoader,
	 const EU4::EU4LocalizationLoader& eu4LocLoader) const
{
	Log(LogLevel::Info) << "-> Converting countries.";

	for (const auto& [tag, country]: countries)
	{
		// this is a freshly-generated decentralized country with no source data whatsoever.
		if (!country->getVanillaData() && country->getProcessedData().type == "decentralized")
			country->generateDecentralizedData(v3LocLoader, eu4LocLoader);

		// this is a vic3-only (vanilla) country with no EU4 match. It's likely extinct.
		else if (country->getVanillaData() && !country->getSourceCountry())
			country->copyVanillaData(v3LocLoader, eu4LocLoader);

		// otherwise, this is a regular imported EU4 country
		else if (country->getSourceCountry())
			country->convertFromEU4Country(clayManager, cultureMapper, religionMapper, cultureLoader, religionLoader, ideaEffectMapper);

		else
			Log(LogLevel::Warning) << "Country " << tag << " has no known sources! Not importing!";
	}
}

bool V3::PoliticalManager::isTagDecentralized(const std::string& v3Tag) const
{
	if (!countries.contains(v3Tag))
		return false;
	const auto& country = countries.at(v3Tag);

	// this means it's loaded from disk, and it's decentralized.
	if (country->getVanillaData() && country->getVanillaData()->type == "decentralized")
		return true;
	return false;
}

std::shared_ptr<V3::Country> V3::PoliticalManager::getCountry(const std::string& v3Tag) const
{
	if (countries.contains(v3Tag))
		return countries.at(v3Tag);
	return nullptr;
}

void V3::PoliticalManager::determineAndApplyWesternization(const mappers::CultureMapper& cultureMapper,
	 const mappers::ReligionMapper& religionMapper,
	 const Configuration::EUROCENTRISM eurocentrism,
	 const DatingData& datingData)
{
	Log(LogLevel::Info) << "-> Determining Westernization.";
	double topTech = 0;
	double topInstitutions = 0;

	// Determine top tech/institutions.
	for (const auto& country: countries | std::views::values)
	{
		if (!country->getSourceCountry())
			continue; // we need only eu4 imports
		const auto& sourceCountry = country->getSourceCountry();
		if (sourceCountry->getProvinces().empty())
			continue; // dead nations are stuck.
		const auto totalTechs =
			 sourceCountry->getMilTech() + sourceCountry->getAdmTech() + sourceCountry->getDipTech() + country->getProcessedData().ideaEffect.getTechMod();
		if (totalTechs > topTech)
			topTech = totalTechs;
		const auto currInstitutions = sourceCountry->getNumEmbracedInstitutions();
		if (currInstitutions > topInstitutions)
			topInstitutions = currInstitutions;
	}

	// and distribute tech level.
	int civs = 0;
	int uncivs = 0;
	for (const auto& country: countries | std::views::values)
	{
		country
			 ->determineWesternizationWealthAndLiteracy(topTech, topInstitutions, cultureMapper, religionMapper, eurocentrism, datingData, populationSetupMapper);

		// Bookkeeping.
		if (!country->getSubStates().empty())
		{
			if (country->getProcessedData().westernized)
				++civs;
			else
				++uncivs;
		}
	}
	Log(LogLevel::Info) << "<> There are " << civs << " westernized and " << uncivs << " unwesternized landed nations.";
}

void V3::PoliticalManager::setupTech()
{
	Log(LogLevel::Info) << "-> Setting up tech for landed EU4 countries.";

	const TechValues techValues(countries);
	auto counter = 0;
	for (const auto& [tag, country]: countries)
	{
		if (!TechValues::isValidCountryForTechConversion(*country))
			continue;

		const auto productionScore = techValues.getProductionTechPercentile(tag);
		const auto militaryScore = techValues.getMilitaryTechPercentile(tag);
		const auto societyScore = techValues.getSocietyTechPercentile(tag);
		country->setTechs(techSetupMapper, productionScore, militaryScore, societyScore);
		++counter;
	}

	Log(LogLevel::Info) << "<> " << counter << " countries initialized.";
}

void V3::PoliticalManager::setupLaws()
{
	Log(LogLevel::Info) << "-> Setting up laws for landed EU4 countries.";
	auto counter = 0;
	for (const auto& country: countries | std::views::values)
	{
		if (!TechValues::isValidCountryForTechConversion(*country))
			continue;

		// ORDER matters! We can set some laws without having other blocking them if we set them first.
		// We go by what's *important*. Political first!
		grantLawFromGroup("lawgroup_governance_principles", country);
		grantLawFromGroup("lawgroup_distribution_of_power", country);
		grantLawFromGroup("lawgroup_slavery", country);
		grantLawFromGroup("lawgroup_citizenship", country);
		grantLawFromGroup("lawgroup_church_and_state", country);
		grantLawFromGroup("lawgroup_economic_system", country);
		grantLawFromGroup("lawgroup_bureaucracy", country);
		grantLawFromGroup("lawgroup_army_model", country);
		grantLawFromGroup("lawgroup_taxation", country);
		grantLawFromGroup("lawgroup_trade_policy", country);
		grantLawFromGroup("lawgroup_internal_security", country);
		grantLawFromGroup("lawgroup_migration", country);
		grantLawFromGroup("lawgroup_policing", country);
		grantLawFromGroup("lawgroup_rights_of_women", country);
		// Social LAST.
		grantLawFromGroup("lawgroup_education_system", country);
		grantLawFromGroup("lawgroup_labor_rights", country);
		grantLawFromGroup("lawgroup_free_speech", country);
		grantLawFromGroup("lawgroup_health_system", country);
		grantLawFromGroup("lawgroup_welfare", country);
		++counter;
	}
	Log(LogLevel::Info) << "<> " << counter << " countries codified.";
}

void V3::PoliticalManager::grantLawFromGroup(const std::string& lawGroup, const std::shared_ptr<Country>& country) const
{
	if (const auto law = lawMapper.grantLawFromGroup(lawGroup, *country); law)
		country->addLaw(*law);
}

void V3::PoliticalManager::convertDiplomacy(const std::vector<EU4::EU4Agreement>& eu4Agreements)
{
	Log(LogLevel::Info) << "-> Transcribing diplomatic agreements.";

	for (auto& agreement: eu4Agreements)
	{
		const auto& EU4Tag1 = agreement.getOriginTag();
		auto ifV3Tag1 = countryMapper->getV3Tag(EU4Tag1);
		if (!ifV3Tag1)
			continue;
		auto V3Tag1 = *ifV3Tag1;

		const auto& EU4Tag2 = agreement.getTargetTag();
		auto ifV3Tag2 = countryMapper->getV3Tag(EU4Tag2);
		if (!ifV3Tag2)
			continue;
		auto V3Tag2 = *ifV3Tag2;

		const auto& country1 = countries.find(V3Tag1);
		const auto& country2 = countries.find(V3Tag2);
		if (country1 == countries.end())
		{
			Log(LogLevel::Warning) << "Vic3 country " << V3Tag1 << " used in diplomatic agreement doesn't exist";
			continue;
		}
		if (country2 == countries.end())
		{
			Log(LogLevel::Warning) << "Vic3 country " << V3Tag2 << " used in diplomatic agreement doesn't exist";
			continue;
		}

		// Don't create for/with nations that didn't survive province conversion!
		if (country1->second->getSubStates().empty())
			continue;
		if (country2->second->getSubStates().empty())
			continue;

		auto& r1 = country1->second->getRelation(V3Tag2); // relation TO target
		auto& r2 = country2->second->getRelation(V3Tag1); // relation TO source

		Agreement newAgreement;
		newAgreement.first = V3Tag1;
		newAgreement.second = V3Tag2;
		newAgreement.start_date = agreement.getStartDate();
		Log(LogLevel::Debug) << agreement.getAgreementType() << " - " << V3Tag1 << "- " << V3Tag2;

		// translate eu4 to vic3 agreement.
		if (const auto& newType = diplomaticMapper.getAgreementType(agreement.getAgreementType()); newType)
			newAgreement.type = *newType;

		// boost relations.
		const auto boost = diplomaticMapper.getRelationshipBoost(agreement.getAgreementType());
		r1.increaseRelations(boost);
		r2.increaseRelations(boost);

		// fix specifics
		if (newAgreement.type == "customs_union")
		{
			// vic3 has this reversed
			newAgreement.first = V3Tag2;
			newAgreement.second = V3Tag1;
		}
		if (newAgreement.type == "double_defensive_pact")
		{
			// fix and file twice.
			newAgreement.type = "defensive_pact";
			agreements.push_back(newAgreement);
			newAgreement.first = V3Tag2;
			newAgreement.second = V3Tag1;
		}

		// store agreement
		if (!newAgreement.type.empty())
			agreements.push_back(newAgreement);
	}
	Log(LogLevel::Info) << "<> Transcribed " << agreements.size() << " agreements.";
}

void V3::PoliticalManager::convertRivals()
{
	Log(LogLevel::Info) << "-> Transcribing rivalries.";
	auto counter = 0;

	for (const auto& [tag, country]: countries)
	{
		if (!country->getSourceCountry())
			continue;
		if (country->getSubStates().empty())
			continue;
		const auto& eu4Rivals = country->getSourceCountry()->getRivals();
		std::set<std::string> newRivals;
		for (const auto& rival: eu4Rivals)
		{
			if (!isEU4CountryConvertedAndLanded(rival))
				continue;

			const auto& rivalTag = countryMapper->getV3Tag(rival);
			const auto& rivalCountry = countries.at(*rivalTag);
			if (rivalCountry->getSubStates().empty())
				continue;

			auto& r1 = country->getRelation(*rivalTag);
			auto& r2 = rivalCountry->getRelation(tag);

			r1.increaseRelations(-100);
			r2.increaseRelations(-100);

			newRivals.emplace(*rivalTag);
			++counter;
		}
		country->setRivals(newRivals); // rivals are one-way, but relations suck both ways.
	}
	Log(LogLevel::Info) << "<> Transcribed " << counter << " rivalries.";
}

void V3::PoliticalManager::convertTruces(const date& lastEU4Date)
{
	Log(LogLevel::Info) << "-> Transcribing truces.";
	auto counter = 0;

	for (const auto& country: countries | std::views::values)
	{
		if (!country->getSourceCountry())
			continue;
		if (country->getSubStates().empty())
			continue;
		for (const auto& [target, relation]: country->getSourceCountry()->getRelations())
		{
			if (!relation.getTruceExpiry())
				continue;
			if (!isEU4CountryConvertedAndLanded(target))
				continue;
			const auto& targetTag = countryMapper->getV3Tag(target);

			auto nominalExpiry = *relation.getTruceExpiry();
			const int conversionDateDays = lastEU4Date.getYear() * 365 + lastEU4Date.getMonth() * 12 + lastEU4Date.getDay();

			const auto remainingTruceDays = nominalExpiry.getYear() * 365 + nominalExpiry.getMonth() * 12 + nominalExpiry.getDay() - conversionDateDays;
			const auto remainingTruceMonths = static_cast<int>(std::round(static_cast<double>(remainingTruceDays) / 30.417)); // let's .. approximate.

			country->addTruce(*targetTag, remainingTruceMonths);
			counter++;
		}
	}
	Log(LogLevel::Info) << "<> Transcribed " << counter << " truces.";
}

bool V3::PoliticalManager::isEU4CountryConvertedAndLanded(const std::string& eu4Tag) const
{
	const auto& targetTag = countryMapper->getV3Tag(eu4Tag);
	if (!targetTag)
		return false;
	if (!countries.contains(*targetTag))
		return false;
	const auto& targetCountry = countries.at(*targetTag);
	if (targetCountry->getSubStates().empty())
		return false;
	return true;
}
