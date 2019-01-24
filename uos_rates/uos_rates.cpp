#include <eosio/uos_rates/uos_rates.hpp>
#include <eosio/uos_rates/transaction_queqe.hpp>
#include <eosio/uos_rates/merkle_tree.hpp>
#include <eosio/uos_rates/data_processor.hpp>
#include <eosio/chain/asset.hpp>
#include <eosio/chain/exceptions.hpp>
#include <eosio/chain_api_plugin/chain_api_plugin.hpp>
#include <eosio/http_plugin/http_plugin.hpp>
#include <../../../libraries/singularity/include/singularity.hpp>
#include <../../../libraries/singularity/generated/git_version.hpp>


#include <fc/io/json.hpp>
#include <fc/crypto/sha256.hpp>
#include <eosio/uos_rates/cvs.h>
#include <algorithm>
#include <boost/program_options.hpp>


namespace eosio {
    using namespace std;

    static appbase::abstract_plugin& _uos_rates = app().register_plugin<uos_rates>();

    class uos_rates_impl {

    public:

        void irreversible_block_catcher(const chain::block_state_ptr& bsp);

        void accepted_block_catcher(const eosio::chain::block_state_ptr& asp);

        fc::variants get_transactions(uint32_t last_block_num);

        vector<fc::variant> get_transactions_from_block(uint32_t block_num);

        void save_userres(uint current_head_block_number);

        map<string, string> get_previous_emission(uint64_t current_calc_block);

        void set_previouos_emission(map<string, string> prev_emission);

        void update_from_snapshot(uint64_t current_calc_block);

        void run_trx_queue(uint64_t num);

        void calculate_rates(uint32_t current_calc_block_num);

        void save_detalization(uos::data_processor dp);

        void save_result();

        void report_hash();

        string get_consensus_leader();

        void set_rates();
        void set_emission();

        vector<std::string> get_all_accounts();

        void run_transaction(
                string account,
                string action,
                fc::mutable_variant_object data,
                string pub_key,
                string priv_key,
                string acc_from = "");

        void run_transaction(transaction_queue &temp);

        void add_transaction(
                string account,
                string action,
                fc::mutable_variant_object data,
                string pub_key,
                string priv_key,
                string acc_from = "");

        void run_transaction(trx_to_run trx){run_transaction(trx.account,trx.action,trx.data,trx.pub_key,trx.priv_key,trx.acc_from);}

        void add_transaction(trx_to_run trx){trx_queue.push(trx);}

        boost::program_options::variables_map _options;

        friend class uos_rates;

        CSVWriter social_activity_log,transfer_activity_log, social_trxs_log;

    private:

        int32_t period = 300*2;
        int32_t window = 86400*2*100;//100 days
        int32_t activity_window = 86400*2*30; //30 days
        string contract_activity = "uos.activity";
        string contract_calculators = "uos.calcs";
        std::set<chain::account_name> calculators;
        string calculator_public_key = "";
        string calculator_private_key = "";
        string calc_contract_public_key = "";
        string calc_contract_private_key = "";


        double social_importance_share = 0.1;
        double transfer_importance_share = 0.1;
        double stake_importance_share = 1.0 - social_importance_share - transfer_importance_share;

        const uint8_t blocks_per_second = 2;
        const uint32_t seconds_per_year = 365*24*3600;
        const double yearly_emission_percent = 1.0;
        const int64_t initial_token_supply = 1000000000;
        const double activity_monetary_value = 1000;

        bool dump_calc_data = false;
        bfs::path dump_dir;

        vector<std::shared_ptr<singularity::relation_t>> my_transfer_interactions;
        uint64_t last_calc_block = 0;
        result_set result = result_set(0); //TODO use pointer

        uint64_t last_setrate_block = 0;

        transaction_queue trx_queue;
    };

    void uos_rates_impl::irreversible_block_catcher(const eosio::chain::block_state_ptr &bsp) {

        //check the latency
        chain::controller &cc = app().get_plugin<chain_plugin>().chain();
        auto ro_api = app().get_plugin<chain_plugin>().get_read_only_api();
        chain_apis::read_only::get_block_params t;
        t.block_num_or_id = cc.fork_db_head_block_id();
        auto head_block = ro_api.get_block(t);
        auto head_block_time_str = head_block["timestamp"].as_string();
        auto head_block_time = fc::time_point::from_iso_string(head_block_time_str);
        auto latency = (fc::time_point::now() - head_block_time).count()/1000;
        if (latency > 1000)
            return;

        //determine current calculating block number
        auto irr_block_id = bsp->block->id();
        auto irr_block_num = bsp->block->num_from_id(irr_block_id);
        auto current_calc_block_num = irr_block_num - (irr_block_num % period);
        ilog(" irreversible block " + to_string(irr_block_num) +
             " last_calc_block " + to_string(last_calc_block) +
             " current_calc_block " + to_string(current_calc_block_num));

        if(last_calc_block < current_calc_block_num) {

            //perform the calculations
            calculate_rates(current_calc_block_num);

            //report the result hash
            report_hash();

            //save the result pack to file
            save_result();

            last_calc_block = current_calc_block_num;
            return;
        }

        if(last_setrate_block < current_calc_block_num)
        {
            //find the consensus leader;
            string leader = get_consensus_leader();
            if (leader == "")
                return;

            //check if we have the leader among our calculators
            if(calculators.find(leader) == calculators.end())
                return;

            //set all rates
            set_rates();

            //emission token for account's with rates
            set_emission();

            last_setrate_block = current_calc_block_num;
            return;
        }

        ilog("waiting for the next calculation block " + std::to_string(current_calc_block_num + period));
    }

    void uos_rates_impl::accepted_block_catcher(const eosio::chain::block_state_ptr &asp)
    {

//        chain::controller &cc = app().get_plugin<chain_plugin>().chain();
//        auto ro_api = app().get_plugin<chain_plugin>().get_read_only_api();
//        chain_apis::read_only::get_block_params t;
        auto current_head_block_number = app().get_plugin<chain_plugin>().chain().fork_db_head_block_num();

        if(current_head_block_number % period == 0) {
            save_userres(current_head_block_number);
            ilog("staked balances snapshot saved for block " + to_string(current_head_block_number));
        }
    }

    fc::variants uos_rates_impl::get_transactions(uint32_t last_block_num) {
        fc::variants result;
        string transactions_cache_file = dump_dir.string() + "/transactions_cache.txt";
        if(bfs::exists(transactions_cache_file)){
            ifstream infile(transactions_cache_file);
            string line;
            while(getline(infile, line)){
                auto variant_json = fc::json::from_string(line);
                result.emplace_back(variant_json);
            }
            infile.close();
        }

        uint32_t start_block = 1;
        if(result.size() > 0) {
            string last_block_str = result.at(result.size()-1)["block_num"].as_string();
            auto last_block =  (uint32_t)stoull(last_block_str);
            start_block = last_block + 1;
        }
        for(uint32_t i = start_block; i <= last_block_num; i++){
            vector<fc::variant> block_trx;

            if(i % 1000000 == 0)
                ilog(to_string(i));

            try { block_trx = get_transactions_from_block(i); }
            catch (...){elog("error on parsing block " + to_string(i));}


            for(auto trx : block_trx){
                result.emplace_back(trx);

                string json = fc::json::to_string(trx);
                ilog(json);
                ofstream outfile(transactions_cache_file, ios_base::app | ios_base::out);
                outfile << json + "\n";
                outfile.close();
            }
        }

        return result;
    }

    vector<fc::variant> uos_rates_impl::get_transactions_from_block(uint32_t block_num){
        vector<fc::variant> result;

        chain::controller &cc = app().get_plugin<chain_plugin>().chain();
        auto block = cc.fetch_block_by_number(block_num);


        auto ro_api = app().get_plugin<chain_plugin>().get_read_only_api();

        for (auto trs : block->transactions) {

            auto transaction = trs.trx.get<chain::packed_transaction>().get_transaction();
            auto actions = transaction.actions;
            for (auto action : actions) {

                if (action.account != eosio::chain::string_to_name(contract_activity.c_str()) &&
                    action.account != N(eosio.token))
                    continue;

                if(action.name != N(usertouser) &&
                   action.name != N(makecontent) &&
                   action.name != N(usertocont) &&
                   action.name != N(makecontorg) &&
                   action.name != N(transfer))
                    continue;

                chain_apis::read_only::abi_bin_to_json_params bins;
                bins.code = action.account;
                bins.action = action.name;
                bins.binargs = action.data;
                auto json = ro_api.abi_bin_to_json(bins);
                auto object = json.args.get_object();

                //serialize social transactions to file
                fc::mutable_variant_object var_trx;
                var_trx["transaction_id"]=fc::variant(transaction.id());
                var_trx["block_num"] = fc::variant(std::to_string(block->block_num()));
                var_trx["acc"] = action.account.to_string();
                var_trx["action"] = action.name.to_string();
                var_trx["data"] = json.args;

                result.emplace_back(var_trx);
            }
        }

        return result;
    }

    void uos_rates_impl::save_userres(uint current_head_block_number) {

        string current_filename = "snapshot_" + to_string(current_head_block_number) + ".csv";
        string file_path = dump_dir.string() + "/" + current_filename;

        if(bfs::exists(file_path))
            bfs::remove(file_path);
        CSVWriter csv_result_userres{current_filename};
        csv_result_userres.settings(true, dump_dir.string(), current_filename);

        vector<string> heading{
                "name",
                "cpu_weight",
                "net_weight"
        };

        csv_result_userres.addDatainRow(heading.begin(), heading.end());

        chain::controller &cc = app().get_plugin<chain_plugin>().chain();
        auto ro_api = app().get_plugin<chain_plugin>().get_read_only_api();
        chain_apis::read_only::get_account_params params;

        auto account_list = get_all_accounts();
        for (auto const &acc_name:  account_list){
            eosio::chain::name bn = acc_name;
            params.account_name = bn;
            asset val;
            auto core_symbol = val.symbol_name();
            try {
                auto users_info = ro_api.get_account(params);
                auto cpu_weight = to_string(users_info.cpu_weight);
                auto net_weight = to_string(users_info.net_weight);
                vector<string> result = {acc_name, cpu_weight, net_weight};
                csv_result_userres.addDatainRow(result.begin(), result.end());
            }
            catch (exception &ex)
            {
                elog(ex.what());
            }

        }
    }

    map<string, string> uos_rates_impl::get_previous_emission(uint64_t current_calc_block) {

        map<string, string> prev_emission;

        auto ro_api = app().get_plugin<chain_plugin>().get_read_only_api();
        {
            ilog("getting cumulative emission from the old table");

            auto accs = get_all_accounts();
            for(auto acc : accs) {
                chain_apis::read_only::get_table_rows_params get_old_em;
                get_old_em.code = eosio::chain::name(contract_calculators);
                get_old_em.scope = acc;
                get_old_em.table = N(account);
                get_old_em.limit = 1;
                get_old_em.json = true;
                auto em_rows = ro_api.get_table_rows(get_old_em);

                if(em_rows.rows.size() == 0)
                    continue;

                string em_str = em_rows.rows[0]["account_sum"].as_string();
                double em_double = stod(em_str);
                stringstream ss;
                ss << fixed << setprecision(4) << em_double;
                string em_str_round =  ss.str();

                prev_emission[acc] = em_str_round;
            }
        }

        return prev_emission;
    }

    void uos_rates_impl::set_previouos_emission(std::map<eosio::string, eosio::string> prev_emission) {
        for(auto item : prev_emission)
        {
            result.res_map[item.first].name = item.first;
            result.res_map[item.first].type = "ACCOUNT";
            result.res_map[item.first].prev_cumulative_emission = item.second;
        }
    }

    void uos_rates_impl::update_from_snapshot(uint64_t current_calc_block)
    {
        //get the staked balance snapshot from the file
        string filename = "snapshot_" + to_string(current_calc_block) + ".csv";
        if(!bfs::exists(dump_dir.string() + "/" + filename)){
            elog("balance snapshot file not found " + filename);
            throw "file not found " + filename;
        }
        auto csv = read_csv_map(dump_dir.string() + "/" + filename);
        auto csv_vect = read_csv(dump_dir.string() + "/" + filename);
        for(auto i = 0; i < csv.size(); i++){
            string name = csv[i]["name"];
            string cpu_weight = csv[i]["cpu_weight"];
            string net_weight = csv[i]["net_weight"];
            auto name_vect = csv_vect[i];
            auto vect = csv_vect[i + 1];

            if(cpu_weight == "-1") cpu_weight = "0";
            if(net_weight == "-1") net_weight = "0";

            if(result.res_map.find(name) == result.res_map.end()){
                result.res_map[name].name = name;
                result.res_map[name].type = "ACCOUNT";
            }

            long total_stake = stol(cpu_weight) + stol(net_weight);
            result.res_map[name].staked_balance = to_string(total_stake);
        }
    }

    void uos_rates_impl::calculate_rates(uint32_t current_calc_block_num) {
        ilog("begin calculate_rates()");

        uos::data_processor dp(current_calc_block_num);

        //input transactions
        auto trxs = get_transactions(current_calc_block_num);
        dp.source_transactions = trxs;

        //input staked balances snapshot
        string snapshot_file = "snapshot_" + to_string(current_calc_block_num) + ".csv";
        auto snapshot_map = read_csv_map(dump_dir.string() + "/" + snapshot_file);
        dp.balance_snapshot = snapshot_map;

        //input previous emission values
        auto prev_emission = get_previous_emission(current_calc_block_num);
        dp.prev_cumulative_emission = prev_emission;

        //interactions format conversion
        dp.convert_transactions_to_relations();

        //calculations of the rates and importance
        dp.calculate_social_rates();
        dp.calculate_transfer_rates();
        dp.calculate_stake_rates();
        dp.calculate_importance();
        dp.calculate_scaled_values();

        //calculations of the emission
        dp.calculate_network_activity();
        dp.calculate_emission();

        //hash
        dp.calculate_hash();


        //convert to the result format
        result = result_set(current_calc_block_num);

        for(auto acc : dp.accounts){
            auto name = acc.first;

            result_item item;
            item.name = name;
            item.type = "ACCOUNT";

            item.soc_rate = dp.get_acc_string_value(name, "social_rate");
            item.soc_rate_scaled = dp.get_acc_string_value(name, "scaled_social_rate");
            item.trans_rate = dp.get_acc_string_value(name, "transfer_rate");
            item.trans_rate_scaled = dp.get_acc_string_value(name, "scaled_transfer_rate");
            item.staked_balance = dp.get_acc_string_value(name, "staked_balance");
            item.stake_rate = dp.get_acc_string_value(name, "stake_rate");
            item.stake_rate_scaled = dp.get_acc_string_value(name, "scaled_stake_rate");
            item.importance = dp.get_acc_string_value(name, "importance");
            item.importance_scaled = dp.get_acc_string_value(name, "scaled_importance");
            item.prev_cumulative_emission = dp.get_acc_string_value(name, "prev_cumulative_emission");
            item.current_emission = dp.get_acc_string_value(name, "current_emission");
            item.current_cumulative_emission = dp.get_acc_string_value(name, "current_cumulative_emission");

            result.res_map[name] = item;
        }

        for(auto cont : dp.content) {
            auto name = cont.first;

            result_item item;
            item.name = name;
            item.type = "CONTENT";

            item.soc_rate = dp.get_cont_string_value(name, "social_rate");
            item.soc_rate_scaled = dp.get_cont_string_value(name, "scaled_social_rate");

            result.res_map[name] = item;
        }

        result.max_activity = dp.max_network_activity;
        result.current_activity = dp.network_activity;
        result.target_emission = dp.target_emission;
        result.emission_limit = dp.emission_limit;

        result.result_hash = dp.result_hash;

        if(dump_calc_data)
            save_detalization(dp);
    }

    void uos_rates_impl::save_detalization(uos::data_processor dp) {
        //index the ownership
        map<string, vector<string>> own_index;
        for(auto relation : dp.social_relations){
            if(relation->get_name() == "OWNERSHIP")
            {
                vector<string> temp_list{relation->get_source(), std::to_string(relation->get_height())};
                own_index[relation->get_target()] = temp_list;
            }
        }

        //index relation with last content id and height
        map<string, map<string, vector<string>>> rel_index;
        map<string, map<string, vector<string>>> cont_index;
        for(auto relation : dp.social_relations){
            if(relation->get_name() != "UPVOTE") continue;
            auto from = relation->get_source();
            auto cont = relation->get_target();
            auto height = std::to_string(relation->get_height());

            vector<string> values{cont, height};
            cont_index[cont][from] = values;

            if(own_index.find(cont) == own_index.end()) continue;
            auto to = own_index[cont][0];

            rel_index[from][to] = values;

        }

        //save social interactions
        auto filename_si = "soc_interactions_" + std::to_string(dp.current_calc_block) + "_" + dp.result_hash + ".txt";
        auto path_si = dump_dir.string() + "/" + filename_si;
        std::remove(path_si.c_str());
        std::ofstream si_file(path_si, std::ios_base::app | std::ios_base::out);
        for(auto si : dp.social_relations) {
            si_file << si->get_name() + ";" +
                       si->get_source() + ";" +
                       si->get_target() + ";" +
                       std::to_string(si->get_height()) + ";" +
                       std::to_string(si->get_weight()) + ";" +
                       std::to_string(si->get_reverse_weight()) + "\n";
        }
        si_file.close();

        //save calculation details
        auto filename = "soc_rate_details_" + std::to_string(dp.current_calc_block) + "_" + dp.result_hash + ".txt";
        auto path = dump_dir.string() + "/" + filename;
        std::remove(path.c_str());
        std::ofstream det_file(path, std::ios_base::app | std::ios_base::out);

        //account rates sorted by rate desc
        multimap<double, string> acc_rates;
        for(auto acc : dp.activity_details.base_index) {
            //accs.emplace_back(acc.first);
            acc_rates.insert({dp.get_acc_double_value(acc.first, "social_rate"), acc.first});
        }
        for(auto acc = acc_rates.rbegin(); acc != acc_rates.rend(); ++acc){
            string name = acc->second;
            auto line = "acc:" + name + " rate:" + dp.get_acc_string_value(name, "social_rate");
            det_file << line + "\n";

            if(dp.activity_details.base_index.find(name) != dp.activity_details.base_index.end()){
                line = "base:" + dp.to_string_10(dp.activity_details.base_index[name]);
                det_file << line + "\n";
            }

            if(dp.activity_details.activity_index_contribution.find(name) !=
               dp.activity_details.activity_index_contribution.end()) {
                //sort upvoters by impact
                multimap<double, string> upvoters_impact;
                for(auto item : dp.activity_details.activity_index_contribution[name]){
                    upvoters_impact.insert({
                                                   stod(dp.to_string_10(item.second.koefficient * item.second.rate)),
                                                   item.first
                    });
                }
                for(auto item = upvoters_impact.rbegin(); item != upvoters_impact.rend(); ++item){
                    auto impact = item->first;
                    auto upname = item->second;
                    auto dets = dp.activity_details.activity_index_contribution[name][upname];
                    line = dp.to_string_10(impact) +
                           ": " + dp.to_string_10(dets.koefficient) +
                           "*" + dp.to_string_10(dets.rate) + " " +
                           "agent:" + upname + " ";
                    if(rel_index.find(upname) != rel_index.end()
                       && rel_index[upname].find(name) != rel_index[upname].end()){
                        line += "cont:" + rel_index[upname][name][0] +
                                " days:" + dp.to_string_10(stod(rel_index[upname][name][1]) / 86400 / 2) +
                                " rel_count:" + std::to_string(rel_index[upname].size());
                    } else {
                        line += "relations not found";
                    }

                    det_file << line + "\n";
                }
            }

            if(dp.activity_details.stack_contribution.find(name) !=
               dp.activity_details.stack_contribution.end()){
                line = "stake: ";
                for(auto item : dp.activity_details.stack_contribution[name]){
                    line += item.first +
                            ":" + dp.to_string_10(item.second.koefficient) +
                            "*" + dp.to_string_10(item.second.rate) + " ";
                }
                det_file << line + "\n";
            }

            det_file << "\n";
        }

        //content rates sorted by rate desc
        multimap<double, string> cont_rates;
        for(auto cont : dp.content) {
            if(dp.get_cont_string_value(cont.first, "social_rate") == "0")
                continue;

            //conts.emplace_back(cont.first);
            cont_rates.insert({dp.get_cont_double_value(cont.first, "social_rate"), cont.first});
        }
        for(auto cont = cont_rates.rbegin(); cont != cont_rates.rend(); ++cont) {
            auto name = cont->second;
            auto line = "cont:" + name +
                        " rate:" + dp.get_cont_string_value(name, "social_rate");
            if(own_index.find(name) != own_index.end()) {
                line += " author:" + own_index[name][0] +
                        " days:" + dp.to_string_10(stod(own_index[name][1]) / 86400 / 2);
            } else {
                line += " no author";
            }

            det_file << line + "\n";

            map<string, vector<string>> upvoters;
            if(cont_index.find(name) == cont_index.end()){
                det_file << "upvoters not found \n";
            } else {
                upvoters = cont_index[name];
            }

            if(dp.content_details.activity_index_contribution.find(name) ==
               dp.content_details.activity_index_contribution.end())
                continue;

            //sort upvoters by impact
            multimap<double, string> upvoters_impact;
            for(auto item : dp.content_details.activity_index_contribution[name]){
                upvoters_impact.insert({
                                               stod(dp.to_string_10(item.second.koefficient * item.second.rate)),
                                               item.first
                                       });
            }

            for(auto item = upvoters_impact.rbegin(); item != upvoters_impact.rend(); ++item) {
                auto impact = item->first;
                auto upname = item->second;
                auto dets = dp.content_details.activity_index_contribution[name][upname];
                line = dp.to_string_10(impact) +
                       ": " + dp.to_string_10(dets.koefficient) +
                       "*" + dp.to_string_10(dets.rate) + " " +
                       "agent:" + upname + " ";
                if(cont_index.find(name) != cont_index.end()
                   && cont_index[name].find(upname) != cont_index[name].end()){
                    line += " days:" + dp.to_string_10(stod(cont_index[name][upname][1]) / 86400 / 2) +
                            " rel_count:" + std::to_string(rel_index[upname].size());
                } else {
                    line += "relations not found";
                }

                det_file << line + "\n";
            }

            det_file << "\n";
        }

        det_file.close();
    }

    void uos_rates_impl::save_result()
    {
        //save to json
        try {
            string json_filename = "result_" + to_string(result.block_num) + "_" + result.result_hash + ".json";
            auto res_var = result.to_variant();
            auto res_json = fc::json::to_pretty_string(res_var);
            fstream json_file;
            json_file.open(dump_dir.string() + "/" + json_filename, ios::out | ios::app);
            json_file << res_json;
            json_file.close();
        }
        catch (exception &ex){ elog(ex.what()); }

        //save to csv
        string filename = "result_" + to_string(result.block_num) + "_" + result.result_hash + ".csv";
        CSVWriter csv_result{filename};
        csv_result.set_write_enabled(true);
        csv_result.set_path(dump_dir.string());
        csv_result.set_filename(filename);

        vector<string> heading{
                "name",
                "type",
                "soc_rate",
                "soc_rate_scaled",
                "trans_rate",
                "trans_rate_scaled",
                "staked_balance",
                "stake_rate",
                "stake_rate_scaled",
                "importance",
                "importance_scaled",
                "current_emission",
                "prev_cumulative_emission",
                "current_cumulative_emission"
        };
        csv_result.addDatainRow(heading.begin(), heading.end());

        for(auto item : result.res_map)
        {
            vector<string> vec{
                item.second.name,
                item.second.type,
                item.second.soc_rate,
                item.second.soc_rate_scaled,
                item.second.trans_rate,
                item.second.trans_rate_scaled,
                item.second.staked_balance,
                item.second.stake_rate,
                item.second.stake_rate_scaled,
                item.second.importance,
                item.second.importance_scaled,
                item.second.current_emission,
                item.second.prev_cumulative_emission,
                item.second.current_cumulative_emission
            };
            csv_result.addDatainRow(vec.begin(), vec.end());
        }
    }


    void uos_rates_impl::report_hash(){

        auto branch = string(GIT_BRANCH);
        auto commit_hash =  string(GIT_COMMIT_HASH);
        auto lib_version = string(SINGULARITY_LIB_VERSION);
        fc::mutable_variant_object version_info;
        version_info["version"] = fc::variant(lib_version);
        version_info["branch"] = fc::variant(branch);
        version_info["commit_hash"] = fc::variant(commit_hash);
        for(auto calc_name : calculators) {
            ilog(calc_name.to_string() + " reportring hash " + result.result_hash +
                 " for block " + to_string(result.block_num));
            fc::mutable_variant_object data;
            data.set("acc", calc_name.to_string());
            data.set("hash", result.result_hash);
            data.set("block_num", result.block_num);
            data.set("memo", fc::json::to_string(version_info));// version lib
            string acc{"acc"};
            add_transaction(contract_calculators, "reporthash", data, calculator_public_key, calculator_private_key,
                            calc_name.to_string());
        }
    }

    string uos_rates_impl::get_consensus_leader(){
        auto ro_api = app().get_plugin<chain_plugin>().get_read_only_api();

        //get list of current calculators
        std::set<string> current_calcs;
        chain_apis::read_only::get_table_rows_params get_calcs;
        get_calcs.code = eosio::chain::name(contract_calculators);
        get_calcs.scope = contract_calculators;
        get_calcs.table = eosio::chain::name("calcreg");
        get_calcs.limit = 100;
        get_calcs.json = true;
        auto calc_rows = ro_api.get_table_rows(get_calcs);
        for(auto cr : calc_rows.rows) {
            string calc_owner = cr["owner"].as_string();
            current_calcs.emplace(calc_owner);
            ilog("calc_owner " + calc_owner);
        }

        //determine minimum 3/4 consensus votes
        int consensus_minimum = (int)std::ceil(current_calcs.size() * 3.0 / 4.0);
        ilog("consensus_minimum " + std::to_string(consensus_minimum));

        //get list of reports for last_calc_block
        vector<fc::variant> reports;
        chain_apis::read_only::get_table_rows_params get_reps;
        get_reps.code = eosio::chain::name(contract_calculators);
        get_reps.scope = contract_calculators;
        get_reps.table = N(reports);
        get_reps.key_type = chain_apis::i64;
        get_reps.index_position = "second";
        get_reps.lower_bound = std::to_string(last_calc_block);
        get_reps.limit = current_calcs.size();
        get_reps.json = true;
        auto rep_rows = ro_api.get_table_rows(get_reps);
        for(auto rr : rep_rows.rows) {
            if(rr["block_num"].as_string() != std::to_string(last_calc_block))
                break;
            reports.push_back(rr);
            ilog("report key:" + rr["key"].as_string() +
                 " acc:" + rr["acc"].as_string() +
                 " hash:" + rr["hash"].as_string() +
                 " block_num:" + rr["block_num"].as_string() +
                 " memo:" + rr["memo"].as_string());
        }

        //count votes for hashes
        std::map<string, int> hash_votes;
        for(auto r : reports) {
            auto hash = r["hash"].as_string();
            ilog("hash vote " + hash);
            ++hash_votes[hash];
        }

        //determine consensus hash
        string consensus_hash = "";
        for(auto hv : hash_votes) {
            if (hv.second >= consensus_minimum)
                consensus_hash = hv.first;
            ilog("hash " + hv.first + " votes " + std::to_string(hv.second));
        }

        //return empty string if no consensus hash
        if(consensus_hash == "") {
            ilog("no consensus hash");
            return "";
        }

        //determine the consensus leader by lowest report key
        uint32_t min_key = std::numeric_limits<uint32_t>::max();
        string leader_acc;
        for(auto r : reports) {
            if(r["hash"].as_string() != consensus_hash)
                continue;

            if(r["key"].as_uint64() >= min_key)
                continue;

            min_key = r["key"].as_uint64();
            leader_acc = r["acc"].as_string();
        }
        ilog("consensus leader " + leader_acc + " key " + std::to_string(min_key));

        return leader_acc;
    }

    void uos_rates_impl::set_rates(){
        //get current rates from the contract table
        auto ro_api = app().get_plugin<chain_plugin>().get_read_only_api();

        ilog("get current rates from the contract table");

        map<string, string> current_rates;

        int page = 100;
        for(int i = 0; i < 1000000; i+=page) {
            chain_apis::read_only::get_table_rows_params get_old_em;
            get_old_em.code = eosio::chain::name(contract_calculators);
            get_old_em.scope = eosio::chain::name(contract_calculators).to_string();
            get_old_em.table = N(rate);
            get_old_em.limit = page;
            get_old_em.lower_bound = std::to_string(i);
            get_old_em.upper_bound = std::to_string(i + 100);
            get_old_em.json = true;
            auto em_rows = ro_api.get_table_rows(get_old_em);

            for (auto row : em_rows.rows) {
                string key_str = row["key"].as_string();
                string name = row["acc_name"].as_string();
                string value = row["value"].as_string();
                //ilog(key_str + " " + name + " " + value);
                current_rates[name] = value;
            }

            if(em_rows.rows.size() == 0) {
                ilog("no more rates found");
                break;
            }
        }

        for(auto item : result.res_map) {
            //skip matching values
            if(current_rates.find(item.second.name) != current_rates.end() &&
                    current_rates[item.second.name] == item.second.soc_rate_scaled){
                //ilog("MATCH " + item.second.name + ":" + current_rates[item.second.name] + "->" + item.second.soc_rate_scaled);
                continue;
            }

            fc::mutable_variant_object data;
            data.set("name", item.second.name);
            data.set("value", item.second.soc_rate_scaled);
            add_transaction(contract_calculators, "setrate", data, calc_contract_public_key, calc_contract_private_key, contract_calculators);
        }
    }

    vector<std::string> uos_rates_impl::get_all_accounts()
    {
        chain::controller &cc = app().get_plugin<chain_plugin>().chain();
        const auto &database = cc.db();
        typedef typename chainbase::get_index_type< chain::account_object >::type index_type;
        const auto &table = database.get_index<index_type,chain::by_name>();
        vector <std::string> account_name;
        for(chain::account_object item: table){
           // ilog(c_fail + "ACCOUNTS:" + item.name.to_string()+ c_clear);
            account_name.push_back(item.name.to_string());
        }
        return account_name;
    }

    void uos_rates_impl::set_emission()
    {
        auto account_list = get_all_accounts();

        for(auto item : result.res_map) {
            //only for real accounts
            if(find(account_list.begin(), account_list.end(), item.second.name) == account_list.end())
                continue;

            fc::mutable_variant_object data;
            data.set("issuer", contract_calculators);
            data.set("receiver", item.second.name);
            data.set("sum", stod(item.second.current_emission));
            data.set("message", "charge for rates " + item.second.name + " " + item.second.current_emission);

            add_transaction(contract_calculators, "addsum", data, calc_contract_public_key, calc_contract_private_key, contract_calculators);
        }

    }

    void uos_rates_impl::run_trx_queue(uint64_t num) {
        if (num == 0)
            return;
        if((fc::time_point::now() - app().get_plugin<chain_plugin>().chain().head_block_header().timestamp)  < fc::seconds(3)) {
            ilog("Run transactions");
            if (trx_queue.empty())
                return;
            trx_to_run last = trx_queue.front();
            transaction_queue temp;
            trx_queue.pop();
            temp.push(last);
            for (uint64_t i = 1; i < num; i++) {
                if (trx_queue.empty())
                    break;
//                ilog("trx_queue.front() " + trx_queue.front().account + " " +
//                             trx_queue.front().acc_from + " " +
//                             trx_queue.front().action + " " +
//                             fc::json::to_string(trx_queue.front().data) + " " +
//                             trx_queue.front().priv_key + " " + " " +
//                             trx_queue.front().pub_key);
                if((last.account==trx_queue.front().account)&&(last.action==trx_queue.front().action)){
                    temp.push(trx_queue.front());
                    trx_queue.pop();
                }
                else{
                    run_transaction(temp);
                    if(temp.size()>0){
                        elog("queue > 0, something went wrong");
                        return;
                    }
                    last = trx_queue.front();
                    temp.push(last);
                    trx_queue.pop();
                }
            }
            run_transaction(temp);
            if(temp.size()>0){
                elog("queue > 0, something went wrong");
                return;
            }
        }
    }

    void uos_rates_impl::add_transaction(
            string account,
            string action,
            fc::mutable_variant_object data,
            string pub_key,
            string priv_key,
            string acc_from)
    {
        trx_queue.emplace(trx_to_run(account,action,data,pub_key,priv_key,acc_from));
    }

    void uos_rates_impl::run_transaction(transaction_queue &temp){
        auto creator_priv_key = fc::crypto::private_key(temp.front().priv_key);
        auto creator_pub_key = fc::crypto::public_key(temp.front().pub_key);
        chain::controller &cc = app().get_plugin<chain_plugin>().chain();

        chain::signed_transaction signed_trx;
        chain::action act;
        chain::abi_serializer eosio_token_serializer;

        auto &accnt = cc.db().get<chain::account_object, chain::by_name>(temp.front().account);
        eosio_token_serializer.set_abi(accnt.get_abi(), fc::milliseconds(100));

        act.name = temp.front().action;//!!!!!!!!!!!!!!! move constants to settings
        act.account = temp.front().account;//!!!!!!!
        act.authorization = vector<chain::permission_level>{{temp.front().acc_from, chain::config::active_name}};

        while(temp.size()) {
            act.data = eosio_token_serializer.variant_to_binary(temp.front().action, temp.front().data, fc::milliseconds(100));
            signed_trx.actions.push_back(act);
            temp.pop();
        }
        signed_trx.expiration = cc.head_block_time() + fc::seconds(5);
        signed_trx.set_reference_block(cc.head_block_id());
        signed_trx.max_net_usage_words = 5000;
        signed_trx.sign(creator_priv_key, cc.get_chain_id());
        try {
            app().get_method<eosio::chain::plugin_interface::incoming::methods::transaction_async>()(
                    std::make_shared<chain::transaction_metadata>(signed_trx),
                    true,
                    [this](const fc::static_variant<fc::exception_ptr, chain::transaction_trace_ptr>& result) -> void{
                        if (result.contains<fc::exception_ptr>()) {
                            elog(fc::json::to_string(result.get<fc::exception_ptr>()));
                        } else {
                            auto trx_trace_ptr = result.get<chain::transaction_trace_ptr>();

                            try {
                                fc::variant pretty_output;
                                pretty_output = app().get_plugin<chain_plugin>().chain().to_variant_with_abi(*trx_trace_ptr, fc::milliseconds(100));
                                ilog("Transaction is sent " + pretty_output["id"].get_string() +
                                     " actions count " + to_string(pretty_output["action_traces"].get_array().size()));
                            }
                            catch (...){
                                elog("Error ");
                            }
                        }
                    });
            ilog("transaction sent ");

        } catch (...) {
            elog("Error in accept transaction");
        }

    }

    void uos_rates_impl::run_transaction(
            string account,
            string action,
            fc::mutable_variant_object data,
            string pub_key,
            string priv_key,
            string acc_from)
    {
        auto creator_priv_key = fc::crypto::private_key(priv_key);
        auto creator_pub_key = fc::crypto::public_key(pub_key);
        chain::controller &cc = app().get_plugin<chain_plugin>().chain();

        chain::signed_transaction signed_trx;
        chain::action act;
        chain::abi_serializer eosio_token_serializer;

        auto &accnt = cc.db().get<chain::account_object, chain::by_name>(account);
        eosio_token_serializer.set_abi(accnt.get_abi(), fc::milliseconds(100));

        act.name = action;//!!!!!!!!!!!!!!! move constants to settings
        act.account = account;//!!!!!!!
        act.authorization = vector<chain::permission_level>{{acc_from, chain::config::active_name}};

        act.data = eosio_token_serializer.variant_to_binary(action, data, fc::milliseconds(100));

        //signed_trx.actions.emplace_back(act);
        signed_trx.actions.push_back(act);

        signed_trx.expiration = cc.head_block_time() + fc::seconds(5);
        signed_trx.set_reference_block(cc.head_block_id());
        signed_trx.max_net_usage_words = 5000;
        signed_trx.sign(creator_priv_key, cc.get_chain_id());

        try {

            app().get_method<eosio::chain::plugin_interface::incoming::methods::transaction_async>()(
                    std::make_shared<chain::transaction_metadata>(signed_trx),
                    true,
                    [this](const fc::static_variant<fc::exception_ptr, chain::transaction_trace_ptr>& result) -> void{
                    if (result.contains<fc::exception_ptr>()) {
                        elog(fc::json::to_string(result.get<fc::exception_ptr>()));
                    } else {
                        auto trx_trace_ptr = result.get<chain::transaction_trace_ptr>();

                        try {
                            fc::variant pretty_output;
                            pretty_output = app().get_plugin<chain_plugin>().chain().to_variant_with_abi(*trx_trace_ptr, fc::milliseconds(100));
                            ilog("Transaction is sent " + pretty_output["id"].get_string() +
                                 " actions count " + to_string(pretty_output["action_traces"].get_array().size()));
                        }
                        catch (...){
                            elog("Error ");
                        }
                    }
            });

            ilog("transaction sent " + action);

        } catch (...) {
            elog("Error in accept transaction");
        }
    }

    uos_rates::uos_rates():my(new uos_rates_impl()){}
    uos_rates::~uos_rates(){}

    void uos_rates::set_program_options(options_description&, options_description& cfg) {
        cfg.add_options()
                ("calculation-period", boost::program_options::value<int32_t>()->default_value(300*2), "Calculation period in blocks")
                ("calculation-window", boost::program_options::value<int32_t>()->default_value(86400*100*2), "Calculation window in blocks")
                ("contract-activity", boost::program_options::value<std::string>()->default_value("uos.activity"), "Contract account to get the input activity")
                ("contract-calculators", boost::program_options::value<std::string>()->default_value("uos.calcs"), "Contract account to get the calculators list")
                ("calculator-name", boost::program_options::value<vector<string>>()->composing()->multitoken(),
                 "ID of calculator controlled by this node (e.g. calc1; may specify multiple times)")
                ("calculator-public-key", boost::program_options::value<std::string>()->default_value(""), "")
                ("calculator-private-key", boost::program_options::value<std::string>()->default_value(""), "")
                ("calc-contract-public-key", boost::program_options::value<std::string>()->default_value(""), "")
                ("calc-contract-private-key", boost::program_options::value<std::string>()->default_value(""), "")
                ("dump-calc-data", boost::program_options::value<bool>()->default_value(false), "Save the input and output data as *.csv files")
                ;
    }

    void uos_rates::plugin_initialize(const variables_map& options) {
        my->_options = &options;

        my->period = options.at("calculation-period").as<int32_t>();
        my->window = options.at("calculation-window").as<int32_t>();
        my->contract_activity = options.at("contract-activity").as<std::string>();
        my->contract_calculators = options.at("contract-calculators").as<std::string>();

        if( options.count("calculator-name") ) {
            const vector<std::string>& ops = options["calculator-name"].as<vector<std::string>>();
            std::copy(ops.begin(), ops.end(), std::inserter(my->calculators, my->calculators.end()));
        }

        my->calculator_public_key = options.at("calculator-public-key").as<std::string>();
        my->calculator_private_key = options.at("calculator-private-key").as<std::string>();
        my->calc_contract_public_key = options.at("calc-contract-public-key").as<std::string>();
        my->calc_contract_private_key = options.at("calc-contract-private-key").as<std::string>();

        my->dump_calc_data = options.at("dump-calc-data").as<bool>();

        auto dump_name = bfs::path("dump");
        my->dump_dir = app().data_dir() / dump_name;
    }

    void uos_rates::plugin_startup() {
        ilog( "starting uos_rates" );

        chain::controller &cc = app().get_plugin<chain_plugin>().chain();

        cc.irreversible_block.connect([this](const auto& bsp){
            my->run_trx_queue(10);
            my->irreversible_block_catcher(bsp);
        });
        cc.accepted_block.connect([this](const auto& asp){
            my->accepted_block_catcher(asp);
        });
    }

    void uos_rates::plugin_shutdown() {
        // OK, that's enough magic
    }

    void uos_rates::irreversible_block_catcher(const eosio::chain::block_state_ptr &bst) { my->irreversible_block_catcher(bst);}

    void uos_rates::accepted_block_catcher(const eosio::chain::block_state_ptr &ast) { my->accepted_block_catcher(ast);}

}
