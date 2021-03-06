#pragma once

#include <eosio/uos_rates/uos_rates.hpp>
//#include <eosio/uos_rates/transaction_queqe.hpp>
#include <eosio/uos_rates/merkle_tree.hpp>
//#include <eosio/chain/asset.hpp>
//#include <eosio/chain/exceptions.hpp>
//#include <eosio/chain_api_plugin/chain_api_plugin.hpp>
//#include <eosio/http_plugin/http_plugin.hpp>
#include <../../../libraries/singularity/include/singularity.hpp>
#include <../../../libraries/singularity/generated/git_version.hpp>
//
//
#include <fc/io/json.hpp>
//#include <fc/crypto/sha256.hpp>
//#include <eosio/uos_rates/cvs.h>
//#include <algorithm>
//#include <boost/program_options.hpp>
//#include "../../../../../../libraries/fc/include/fc/variant.hpp"
typedef boost::multiprecision::number< boost::multiprecision::cpp_dec_float<10> > double_type;
typedef std::shared_ptr<singularity::relation_t> p_sing_relation_t;


namespace uos {
    using namespace std;
    using namespace eosio;

    class data_processor {

    public:

        //settings
        int32_t period = 5*60*2;//5 minutes
        uint32_t transaction_window = 100*86400*2;//100 days
        uint32_t activity_window = 30*86400*2; //30 days

        int64_t ref_period = 365*24*60*60*2; //time_referrals
        double ref_share = 0.1;

        const double activity_monetary_value = 10000;
        const uint8_t blocks_per_second = 2;
        const double yearly_emission_percent = 0.2;
        const int64_t initial_token_supply = 1000000000;

        //input
        uint32_t current_calc_block;
        std::string current_calc_block_time;
        fc::variants source_transactions;
        vector<map<string,string>> balance_snapshot;
        map<string,string> prev_cumulative_emission;
        string prev_max_network_activity = "0";

        //intermediate
        int32_t start_block;
        int32_t end_block;
        int32_t activity_start_block;
        int32_t activity_end_block;

        long total_stake = 0;
        vector<std::shared_ptr<singularity::relation_t>> transfer_relations;
        vector<std::shared_ptr<singularity::relation_t>> social_relations;
        vector<std::shared_ptr<singularity::relation_t>> trust_relations;//new type relations
        map<string,vector<p_sing_relation_t>> common_relations;//trust and reference

        vector<std::shared_ptr<singularity::relation_t>> activity_relations;

        //trx rejects from history
        map<std::string,std::vector<std::string>> trx_rejects;

        //output
        map<string, fc::mutable_variant_object> accounts;
        map<string, fc::mutable_variant_object> content;

        //calculation details
        singularity::activity_index_detalization_t activity_details;
        singularity::activity_index_detalization_t priority_details;
        singularity::activity_index_detalization_t content_details;
        singularity::intermediate_results_t intermediate_results;

        string network_activity;
        string max_network_activity;
        string full_prev_emission;
        string target_emission;
        string emission_limit;
        string resulting_emission;
        string real_resulting_emission;

        set<string> st_make_id_contents;//unique
        set<string> reference_trx;//unique
        string result_hash;

        explicit data_processor(uint32_t calc_block, std::string calc_block_time){
            current_calc_block = calc_block;
            current_calc_block_time = calc_block_time;
        }

        void prepare_actor_ids();

        void set_block_limits();

        void process_transaction_history();
        void process_old_social_transaction(fc::variant trx);
        int32_t convert_time_to_block_num(std::string str_time);
        void process_generic_social_transaction(fc::variant trx);
        void process_transfer_transaction(fc::variant trx);

        void add_organization( std::string creator, std::string organization, int32_t block);
        void add_content( std::string author, std::string content, int32_t block);
        void add_upvote( std::string from, std::string to, int32_t block);
        void add_downvote( std::string from, std::string to, int32_t block);
        void add_transfer( std::string from, std::string to, uint64_t quantity, int32_t block);
        void add_trust( std::string from, std::string to, int32_t block);
        void add_referral( std::string from, std::string to, int32_t block);

        void calculate_social_rates();
        void set_intermediate_results();
        void calculate_transfer_rates();
        void calculate_stake_rates();
        void calculate_importance(double social_importance_share,double transfer_importance_share);
        void calculate_referrals();

        void calculate_scaled_values();

        void calculate_network_activity();

        void calculate_emission();

        uos::merkle_tree<string> calculate_hash();

        static string to_string_4(double value);
        static string to_string_4(singularity::double_type value);
        static string to_string_10(double value);
        static string to_string_10(singularity::double_type value);

        string get_acc_string_value(string acc_name, string value_name);
        string get_acc_string_4_value(string acc_name, string value_name);
        string get_acc_string_10_value(string acc_name, string value_name);
        double get_acc_double_value(string acc_name, string value_name);
        long get_acc_long_value(string acc_name, string value_name);
        string get_cont_string_value(string cont_name, string value_name);
        string get_cont_string_10_value(string cont_name, string value_name);
        double get_cont_double_value(string cont_name, string value_name);
    };

    void data_processor::prepare_actor_ids() {
        for(auto item : balance_snapshot) {
            if(accounts.find(item["name"]) == accounts.end()){
                accounts[item["name"]].set("origin", "balance");
            }

            string cpu_weight = item["cpu_weight"];
            string net_weight = item["net_weight"];

            if(cpu_weight == "-1") cpu_weight = "0";
            if(net_weight == "-1") net_weight = "0";
            long staked_balance = stol(cpu_weight) + stol(net_weight);
            total_stake += staked_balance;

            accounts[item["name"]].set("staked_balance", std::to_string(staked_balance));
        }
    }

    void data_processor::set_block_limits() {
        end_block = current_calc_block;
        start_block = end_block - transaction_window + 1;

        activity_end_block = end_block;
        activity_start_block = end_block - activity_window + 1;
    }

    void data_processor::process_transaction_history() {
        for(auto trx : source_transactions){
            try {
                //reject if transaction contains \n
                auto json = fc::json::to_string(trx);
                if(json.find("\n") != std::string::npos ||
                   json.find("\\n") != std::string::npos ||
                   json.find("\r") != std::string::npos ||
                   json.find("\\r") != std::string::npos){
                    trx_rejects["newline"].push_back(json);
                    continue;
                }

                if(trx["acc"].as_string() == "uos.activity" &&
                   trx["action"].as_string() != "socialaction" &&
                   trx["action"].as_string() != "socialactndt") {
                    process_old_social_transaction(trx);
                }
                if(trx["acc"].as_string() == "uos.activity" &&
                   (trx["action"].as_string() == "socialaction" || 
                    trx["action"].as_string() == "socialactndt" ||
                    trx["action"].as_string() == "histactndt")) {
                    process_generic_social_transaction(trx);
                }
                if(trx["acc"].as_string() == "eosio.token") {
                    process_transfer_transaction(trx);
                }
            }
            catch (std::exception &ex){
                trx_rejects["parsing_error"].push_back(fc::json::to_string(trx));
                elog(ex.what());
                }
            catch (...){
                trx_rejects["parsing_error"].push_back(fc::json::to_string(trx));
            }
        }
    }

    void data_processor::process_old_social_transaction(fc::variant trx) {
        
        auto block_num = stoi(trx["block_num"].as_string());

        if (trx["action"].as_string() == "makecontent" && trx["data"]["content_type_id"].as_string() == "4" ) {
            add_organization(
                trx["data"]["acc"].as_string(),
                trx["data"]["content_id"].as_string(),
                block_num);
        }

        if (trx["action"].as_string() == "makecontent" && trx["data"]["content_type_id"].as_string() != "4" ) {
            add_content(
                trx["data"]["acc"].as_string(),
                trx["data"]["content_id"].as_string(),
                block_num);
        }

        if (trx["action"].as_string() == "usertocont" && trx["data"]["interaction_type_id"].as_string() == "2") {
            add_upvote(
                trx["data"]["acc"].as_string(),
                trx["data"]["content_id"].as_string(),
                block_num);
        }

        if (trx["action"].as_string() == "usertocont" && trx["data"]["interaction_type_id"].as_string() == "4") {
            add_downvote(
                trx["data"]["acc"].as_string(),
                trx["data"]["content_id"].as_string(),
                block_num);
        }
        
        if (trx["action"].as_string() == "makecontorg") {
            add_content(
                trx["data"]["organization_id"].as_string(),
                trx["data"]["content_id"].as_string(),
                block_num);
        }
    }

    int32_t data_processor::convert_time_to_block_num(std::string str_time) {
        str_time.pop_back();
        str_time = str_time + ".000";
        //ilog(str_time);
        //ilog(current_calc_block_time);
        int32_t history_block_time = fc::time_point::from_iso_string(str_time).sec_since_epoch();
        int32_t current_block_time = fc::time_point::from_iso_string(current_calc_block_time).sec_since_epoch();
        int32_t difference = current_block_time - history_block_time;
        //ilog(current_calc_block_time + " " + str_time + " " + std::to_string(difference));
        return - difference * 2;
    }

    void data_processor::process_generic_social_transaction(fc::variant trx) {
        
        auto block_num = stoi(trx["block_num"].as_string());
        auto from = trx["data"]["acc"].as_string();

        if(trx["action"].as_string() == "histactndt") {
            block_num = convert_time_to_block_num(trx["data"]["timestamp"].as_string());
        }
        
        auto action_json = trx["data"]["action_json"].as_string();
        auto json_data = fc::json::from_string(action_json);

        if(json_data["interaction"] == "create_media_post_from_account") {
            add_content(
                json_data["data"]["account_from"].as_string(),
                json_data["data"]["content_id"].as_string(),
                block_num);
        } else if(json_data["interaction"] == "create_media_post_from_organization") {
            add_content(
                json_data["data"]["organization_id_from"].as_string(),
                json_data["data"]["content_id"].as_string(),
                block_num);
        } else if(json_data["interaction"] == "upvote") {
            add_upvote(
                json_data["data"]["account_from"].as_string(),
                json_data["data"]["content_id"].as_string(),
                block_num);
        } else if(json_data["interaction"] == "downvote") {
            add_downvote(
                json_data["data"]["account_from"].as_string(),
                json_data["data"]["content_id"].as_string(),
                block_num);
        } else if(json_data["interaction"] == "create_organization") {
            add_organization(
                json_data["data"]["account_from"].as_string(),
                json_data["data"]["organization_id"].as_string(),
                block_num);
        } else if(json_data["interaction"] == "trust") {
            add_trust(
                json_data["data"]["account_from"].as_string(),
                json_data["data"]["account_to"].as_string(),
                block_num);
        } else if(json_data["interaction"] == "reference") {
            add_referral(
                json_data["data"]["account_from"].as_string(),
                json_data["data"]["account_to"].as_string(),
                block_num);
        } else if(json_data["interaction"] == "referral") {
            add_referral(
                json_data["data"]["account_from"].as_string(),
                json_data["data"]["account_to"].as_string(),
                block_num);
        } else {
            trx_rejects["unused_generics"].push_back(fc::json::to_string(trx));
        }
    }

    void data_processor::process_transfer_transaction(fc::variant trx) {
        
        auto block_num = stoi(trx["block_num"].as_string());

        add_transfer(
            trx["data"]["from"].as_string(),
            trx["data"]["to"].as_string(),
            asset::from_string(trx["data"]["quantity"].as_string()).get_amount(),
            block_num);
    }

    void data_processor::add_organization(
        std::string creator,
        std::string organization,
        int32_t block) {
            
            if(content.find(organization) != content.end()) {
                trx_rejects["wrong_actor"].push_back(std::to_string(block) + "_make_organization_" + organization);
                return;
            }
            
            if(accounts.find(organization) == accounts.end()){
                accounts[organization].set("origin", "make_organization");
            }
    }

    void data_processor::add_content(
        std::string author,
        std::string content_id,
        int32_t block) {
            
            if(accounts.find(content_id) != accounts.end()){
                trx_rejects["wrong_content"].push_back(std::to_string(block) + "_make_content_" + content_id);
                return;
            }

            if(content.find(author) != content.end()){
                trx_rejects["wrong_actor"].push_back(std::to_string(block) + "_make_content_" + author);
                return;
            }

            if(content.find(content_id) != content.end()){
                trx_rejects["duplicate_content_ownership"].push_back(std::to_string(block) + "_" + content_id + "_" + author);
                return;
            }

            if(accounts.find(author) == accounts.end()){
                accounts[author].set("origin", "make_content");
            }

            content[content_id].set("origin", "make_content");  

            if(start_block < block && block <= end_block) {
                ownership_t ownership(author, content_id, current_calc_block - block);
                social_relations.push_back(std::make_shared<ownership_t>(ownership));
            }

            if(activity_start_block < block && block <= activity_end_block) {
                ownership_t ownership(author, content_id, current_calc_block - block);
                activity_relations.push_back(std::make_shared<ownership_t>(ownership));
            }
    }

    void data_processor::add_upvote(
        std::string from,
        std::string to,
        int32_t block) {
            
            if(accounts.find(to) != accounts.end()) {
                trx_rejects["wrong_content"].push_back(std::to_string(block) + "_upvote_" + to);
                return;
            }

            if(content.find(from) != content.end()) {
                trx_rejects["wrong_actor"].push_back(std::to_string(block) + "_upvote_" + from);
                return;
            }

            if(accounts.find(from) == accounts.end()) {
                accounts[from].set("origin", "upvote");
            }

            if(content.find(to) == content.end()) {
                content[to].set("origin", "upvote");
            }

            if(start_block < block && block <= end_block) {
                upvote_t upvote(from, to, current_calc_block - block);
                social_relations.push_back(std::make_shared<upvote_t>(upvote));
            }

            if(activity_start_block < block && block <= activity_end_block) {
                upvote_t upvote(from, to, current_calc_block - block);
                activity_relations.push_back(std::make_shared<upvote_t>(upvote));
            }
    }

    void data_processor::add_downvote(
        std::string from,
        std::string to,
        int32_t block) {

            if(accounts.find(to) != accounts.end()) {
                trx_rejects["wrong_content"].push_back(std::to_string(block) + "_downvote_" + to);
                return;
            }

            if(content.find(from) != content.end()) {
                trx_rejects["wrong_actor"].push_back(std::to_string(block) + "_downvote_" + from);
                return;
            }

            if(accounts.find(from) == accounts.end()) {
                accounts[from].set("origin", "downvote");
            }

            if(content.find(to) == content.end()) {
                content[to].set("origin", "downvote");
            }

            if(start_block < block && block <= end_block) {
                downvote_t downvote(from, to, current_calc_block - block);
                social_relations.push_back(std::make_shared<downvote_t>(downvote));
            }

            if(activity_start_block < block && block <= activity_end_block) {
                downvote_t downvote(from, to, current_calc_block - block);
                activity_relations.push_back(std::make_shared<downvote_t>(downvote));
            }
    }
    
    void data_processor::add_transfer(
        std::string from,
        std::string to,
        uint64_t quantity,
        int32_t block) {

            if(start_block < block && block <= end_block) {
                transaction_t transfer(quantity,from, to,0 , current_calc_block - block);
                transfer_relations.push_back(std::make_shared<transaction_t>(transfer));
            }

            if(activity_start_block < block && block <= activity_end_block) {
                transaction_t transfer(quantity,from, to,0 , current_calc_block - block);
                activity_relations.push_back(std::make_shared<transaction_t>(transfer));
            }
    }

    void data_processor::add_trust(
        std::string from,
        std::string to,
        int32_t block) {
            if(accounts.find(from) == accounts.end()) {
                trx_rejects["wrong_actor"].push_back(std::to_string(block) + "_trust_" + from);
                return;
            }

            if(accounts.find(to) == accounts.end()) {
                trx_rejects["wrong_actor"].push_back(std::to_string(block) + "_trust_" + to);
                return;
            }

            trust_t trust(from, to, current_calc_block - block);
            common_relations["trust"].push_back(std::make_shared<trust_t>(trust));
    }

    void data_processor::add_referral(
        std::string from,
        std::string to,
        int32_t block){
            if(accounts.find(from) == accounts.end()) {
                trx_rejects["wrong_actor"].push_back(std::to_string(block) + "_referral_" + from);
                return;
            }

            if(accounts.find(to) == accounts.end()) {
                trx_rejects["wrong_actor"].push_back(std::to_string(block) + "_referral_" + to);
                return;
            }

            reference_t reference(from, to, current_calc_block - block);
            common_relations["reference"].push_back(std::make_shared<reference_t>(reference));            
    }

    void data_processor::calculate_social_rates() {
        singularity::parameters_t params;
        params.include_detailed_data = true;
        params.use_diagonal_elements = true;
        params.stack_contribution = 0;
        params.weight_contribution = 1;

        map <string, double_type> stake;
        for(auto item: accounts){
            double staked_balance = get_acc_double_value(item.first, "staked_balance");
            stake.insert(std::pair<string, double_type>(item.first, (double_type)staked_balance));
        }

        auto social_calculator =
                singularity::rank_calculator_factory::create_calculator_for_social_network(params);

        social_calculator->add_stack_vector(stake);

        social_calculator->add_block(social_relations);
        social_calculator->add_block(common_relations["trust"]);
        auto social_rates = social_calculator->calculate();

        for(auto item : *social_rates[singularity::ACCOUNT]){
            if(accounts.find(item.first) == accounts.end())
                accounts[item.first] = fc::mutable_variant_object();
            accounts[item.first].set("social_rate", to_string_10(item.second));
        }

        for(auto item : *social_rates[singularity::CONTENT]){
            if(content.find(item.first) == content.end())
                content[item.first] = fc::mutable_variant_object();
            content[item.first].set("social_rate", to_string_10(item.second));
        }

        activity_details = social_calculator->get_account_rank_detalization();
        priority_details = social_calculator->get_account_priority_detalization();
        content_details = social_calculator->get_content_rank_detalization();

        intermediate_results = social_calculator->get_last_intermediate_results();
    }

    void data_processor::set_intermediate_results(){
        for(auto item : accounts){
            auto name = item.first;
            
            auto di = intermediate_results.default_initial.find(name);
            if(di != intermediate_results.default_initial.end()){
                accounts[name].set("default_initial", to_string_10(di->second));
            }

            auto tr = intermediate_results.trust.find(name);
            if(tr != intermediate_results.trust.end()){
                accounts[name].set("trust", to_string_10(tr->second));
            }

            auto pr = intermediate_results.priority.find(name);
            if(pr != intermediate_results.priority.end()){
                accounts[name].set("priority", to_string_10(pr->second));
            }

            auto st = intermediate_results.stack.find(name);
            if(st != intermediate_results.stack.end()){
                accounts[name].set("stack", to_string_10(st->second));
            }
        }
    }

    void data_processor::calculate_transfer_rates() {
        singularity::parameters_t params;

        auto transfer_calculator =
                singularity::rank_calculator_factory::create_calculator_for_transfer(params);

        transfer_calculator->add_block(transfer_relations);
        auto transfer_rates = transfer_calculator->calculate();

        if(transfer_rates.size() == 0) {
            return;
        }

        for(auto item : *transfer_rates[singularity::ACCOUNT]){
            if(accounts.find(item.first) == accounts.end())
                accounts[item.first] = fc::mutable_variant_object();
            accounts[item.first].set("transfer_rate", to_string_10(item.second));
        }
    }

    void data_processor::calculate_stake_rates() {

        for(auto acc : accounts){
            double stake_rate = get_acc_double_value(acc.first,"staked_balance") / (double) total_stake;
            accounts[acc.first].set("stake_rate", to_string_10(stake_rate));
        }
    }

    void data_processor::calculate_importance(double social_importance_share,double transfer_importance_share) {
        if((social_importance_share + transfer_importance_share) > 1)
        {
            social_importance_share = 0.4;
            transfer_importance_share = 0.1;
            elog("Summa social_importance_share and transfer_importance_share more than 1.Set default value 0.1 and 0.1 ");
        }

        double stake_importance_share = 1-(social_importance_share + transfer_importance_share);

        for (auto item : accounts){
            double importance = get_acc_double_value(item.first, "social_rate") * social_importance_share +
                                get_acc_double_value(item.first, "transfer_rate") * transfer_importance_share +
                                get_acc_double_value(item.first, "stake_rate") * stake_importance_share;
            accounts[item.first].set("importance", to_string_10(importance));
        }
    }


    void data_processor::calculate_referrals()
    {

        auto it = common_relations.find("reference");
        if (it != common_relations.end()) {
            auto rel_ref = it->second;
            for(auto ref:rel_ref) {
                accounts[ref->get_source()].set("referal", ref->get_target());
                uint64_t height = ref->get_height();

                auto fading = (height >= ref_period) ? 0 : ((double)(ref_period - height)/(double)ref_period);

                double importance_refer = get_acc_double_value(ref->get_source(), "importance");
                double referal_bonus = ref_share * importance_refer * fading;
                double importance_referal_new = get_acc_double_value(ref->get_target(), "importance")+ referal_bonus;

                accounts[ref->get_target()].set("importance", importance_referal_new);
                ilog("REFERAL BONUS add:" + ref->get_target() + string(":") + to_string_10(referal_bonus));

                double importance_referals_new = importance_refer - referal_bonus;

                accounts[ref->get_source()].set("importance", importance_referals_new);
                accounts[ref->get_source()].set("referal_bonus", referal_bonus);
                ilog("REFERALS BONUS remove:" + ref->get_source() + string(":") + to_string_10(referal_bonus));

            }
        }

    }

    void data_processor::calculate_scaled_values() {
        //auto acc_count = accounts.size();
        ///for acc_count use only accounts with non-zero social_rate
        int acc_count = 0;
        for(auto acc : accounts){
            if(get_acc_string_value(acc.first, "social_rate") != "0")
                acc_count++;
        }
        for(auto acc : accounts){
            double scaled_social_rate = get_acc_double_value(acc.first, "social_rate") * acc_count;
            double scaled_transfer_rate = get_acc_double_value(acc.first, "transfer_rate") * acc_count;
            double scaled_stake_rate = get_acc_double_value(acc.first, "stake_rate") * acc_count;
            double scaled_importance = get_acc_double_value(acc.first, "importance") * acc_count;

            accounts[acc.first].set("scaled_social_rate", to_string_10(scaled_social_rate));
            accounts[acc.first].set("scaled_transfer_rate", to_string_10(scaled_transfer_rate));
            accounts[acc.first].set("scaled_stake_rate", to_string_10(scaled_stake_rate));
            accounts[acc.first].set("scaled_importance", to_string_10(scaled_importance));
        }

        //auto cont_count = content.size();
        ///use the same scale for the content rate
        for(auto cont : content){
            double scaled_social_rate = get_cont_double_value(cont.first, "social_rate") * acc_count;
            content[cont.first].set("scaled_social_rate", to_string_10(scaled_social_rate));
        }
    }

    void data_processor::calculate_network_activity() {
        singularity::activity_period_new act_period(activity_window, 1);
        act_period.add_block(activity_relations);
        //auto activity = act_period.get_activity();
        auto activity = (double) activity_relations.size();
        network_activity = to_string_10(activity);

        double max_activity_d = stod(prev_max_network_activity);
        if(stod(to_string_10(activity)) > max_activity_d)
            max_activity_d = stod(to_string_10(activity));
        max_network_activity = to_string_10(max_activity_d);
    }

    void data_processor::calculate_emission() {
        singularity::emission_parameters_t params;
        params.yearly_emission_percent = yearly_emission_percent;
        params.emission_period_seconds = period / blocks_per_second;
        params.activity_monetary_value = activity_monetary_value;
        params.delay_koefficient = 0.5;
        singularity::emission_calculator em_calculator(params);

        auto target_emission_d = em_calculator.get_target_emission(stod(network_activity), 0);
        target_emission = to_string_4(target_emission_d);
        auto emission_limit_d = em_calculator.get_emission_limit(initial_token_supply);
        emission_limit = to_string_4(emission_limit_d);

        double full_prev_emission_d = 0;
        for(auto item : prev_cumulative_emission){
            if(accounts.find(item.first) == accounts.end())
                accounts[item.first] = fc::mutable_variant_object();
            accounts[item.first].set("prev_cumulative_emission", item.second);
            full_prev_emission_d += stod(item.second);
        }
        full_prev_emission = to_string_4(full_prev_emission_d);

        auto resulting_emission_d = em_calculator.get_resulting_emission(
                stod(target_emission) - full_prev_emission_d, stod(emission_limit));
        resulting_emission = to_string_4(resulting_emission_d);


        double real_resulting_emission_d = 0;
        for(auto acc : accounts){
            double current_emission_d = stod(resulting_emission) * get_acc_double_value(acc.first, "importance");
            accounts[acc.first].set("current_emission", to_string_4(current_emission_d));
            double cumulative_emission = get_acc_double_value(acc.first, "prev_cumulative_emission") +
                                         get_acc_double_value(acc.first, "current_emission");
            accounts[acc.first].set("current_cumulative_emission", to_string_4(cumulative_emission));

            real_resulting_emission_d += get_acc_double_value(acc.first, "current_emission");
        }
        real_resulting_emission = to_string_4(real_resulting_emission_d);
    }

    uos::merkle_tree<string> data_processor::calculate_hash() {
        uos::merkle_tree<string> mtree;
        vector< pair< string, string> > mt_input;
        for(auto acc : accounts){

            string str_statement = "emission " + acc.first +
                                   " " + get_acc_string_value(acc.first, "current_cumulative_emission");
            mt_input.emplace_back(make_pair(str_statement, str_statement));
            
            string str_importance = "importance " + acc.first +
                                   " " + get_acc_string_value(acc.first, "importance");
            mt_input.emplace_back(make_pair(str_importance, str_importance));
            
        }
        mtree.set_accounts(mt_input);
        mtree.count_tree();
        result_hash = string(mtree.nodes_list[mtree.nodes_list.size() - 1][0]);

        return mtree;
    }

    string data_processor::to_string_4(double value) {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(4) << value;
        return ss.str();
    }

    string data_processor::to_string_4(singularity::double_type value) {
        return value.str(4,ios_base::fixed);
    }

    string data_processor::to_string_10(double value) {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(10) << value;
        return ss.str();
    }

    string data_processor::to_string_10(singularity::double_type value) {
        return value.str(10,ios_base::fixed);
    }

    string data_processor::get_acc_string_value(std::string acc_name, std::string value_name) {
        if(accounts.find(acc_name) == accounts.end())
            return "0";

        if(accounts[acc_name].find(value_name) == accounts[acc_name].end())
            return "0";

        auto str_value = accounts[acc_name][value_name].as_string();
        return str_value;
    }

    string data_processor::get_acc_string_4_value(std::string acc_name, std::string value_name) {
        auto double_value = get_acc_double_value(acc_name, value_name);
        return to_string_4(double_value);
    }

    string data_processor::get_acc_string_10_value(std::string acc_name, std::string value_name) {
        auto double_value = get_acc_double_value(acc_name, value_name);
        return to_string_10(double_value);
    }

    double data_processor::get_acc_double_value(std::string acc_name, std::string value_name) {
        auto str_value = get_acc_string_value(acc_name, value_name);
        return stod(str_value);
    }

    long data_processor::get_acc_long_value(std::string acc_name, std::string value_name) {
        auto str_value = get_acc_string_value(acc_name, value_name);
        return stol(str_value);
    }

    string data_processor::get_cont_string_value(std::string cont_name, std::string value_name) {
        if(content.find(cont_name) == content.end())
            return "0";

        if(content[cont_name].find(value_name) == content[cont_name].end())
            return "0";

        auto str_value = content[cont_name][value_name].as_string();
        return str_value;
    }

    string data_processor::get_cont_string_10_value(std::string cont_name, std::string value_name) {
        auto double_value = get_cont_double_value(cont_name, value_name);
        return to_string_10(double_value);
    }

    double data_processor::get_cont_double_value(std::string cont_name, std::string value_name) {
        auto str_value = get_cont_string_value(cont_name, value_name);
        return stod(str_value);
    }
}

