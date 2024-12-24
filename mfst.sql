-- MySQL dump 10.19  Distrib 10.3.39-MariaDB, for debian-linux-gnu (x86_64)
--
-- Host: localhost    Database: mfst
-- ------------------------------------------------------
-- Server version	10.3.39-MariaDB-0ubuntu0.20.04.2

/*!40101 SET @OLD_CHARACTER_SET_CLIENT=@@CHARACTER_SET_CLIENT */;
/*!40101 SET @OLD_CHARACTER_SET_RESULTS=@@CHARACTER_SET_RESULTS */;
/*!40101 SET @OLD_COLLATION_CONNECTION=@@COLLATION_CONNECTION */;
/*!40101 SET NAMES utf8mb4 */;
/*!40103 SET @OLD_TIME_ZONE=@@TIME_ZONE */;
/*!40103 SET TIME_ZONE='+00:00' */;
/*!40014 SET @OLD_UNIQUE_CHECKS=@@UNIQUE_CHECKS, UNIQUE_CHECKS=0 */;
/*!40014 SET @OLD_FOREIGN_KEY_CHECKS=@@FOREIGN_KEY_CHECKS, FOREIGN_KEY_CHECKS=0 */;
/*!40101 SET @OLD_SQL_MODE=@@SQL_MODE, SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */;
/*!40111 SET @OLD_SQL_NOTES=@@SQL_NOTES, SQL_NOTES=0 */;

--
-- Table structure for table `cards`
--

DROP TABLE IF EXISTS `cards`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8 */;
CREATE TABLE `cards` (
  `id` bigint(20) unsigned NOT NULL AUTO_INCREMENT,
  `name` varchar(256) DEFAULT NULL,
  `uuid` char(36) DEFAULT NULL,
  `size` bigint(20) unsigned DEFAULT NULL,
  `sector_size` int(10) unsigned DEFAULT NULL,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uuid_2` (`uuid`),
  KEY `uuid` (`uuid`)
) ENGINE=InnoDB AUTO_INCREMENT=162 DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Table structure for table `consolidated_sector_maps`
--

DROP TABLE IF EXISTS `consolidated_sector_maps`;
/*!40101 SET @saved_cs_client     = @@character_set_client */;
/*!40101 SET character_set_client = utf8 */;
CREATE TABLE `consolidated_sector_maps` (
  `id` bigint(20) unsigned NOT NULL,
  `is_active` tinyint(1) unsigned NOT NULL DEFAULT 1,
  `cur_round_num` bigint(20) unsigned DEFAULT NULL,
  `round_num_offset` bigint(20) unsigned DEFAULT 0,
  `num_bad_sectors` bigint(20) unsigned DEFAULT NULL,
  `consolidated_sector_map` blob DEFAULT NULL,
  `status` int(11) DEFAULT NULL,
  `rate` double DEFAULT NULL,
  `last_updated` bigint(20) unsigned DEFAULT NULL,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci;
/*!40101 SET character_set_client = @saved_cs_client */;

--
-- Temporary table structure for view `endurance_test_data`
--

DROP TABLE IF EXISTS `endurance_test_data`;
/*!50001 DROP VIEW IF EXISTS `endurance_test_data`*/;
SET @saved_cs_client     = @@character_set_client;
SET character_set_client = utf8;
/*!50001 CREATE VIEW `endurance_test_data` AS SELECT
 1 AS `id`,
  1 AS `name`,
  1 AS `size`,
  1 AS `sector_size`,
  1 AS `cur_round_num`,
  1 AS `num_bad_sectors`,
  1 AS `consolidated_sector_map`,
  1 AS `status`,
  1 AS `rate`,
  1 AS `last_updated` */;
SET character_set_client = @saved_cs_client;

--
-- Final view structure for view `endurance_test_data`
--

/*!50001 DROP VIEW IF EXISTS `endurance_test_data`*/;
/*!50001 SET @saved_cs_client          = @@character_set_client */;
/*!50001 SET @saved_cs_results         = @@character_set_results */;
/*!50001 SET @saved_col_connection     = @@collation_connection */;
/*!50001 SET character_set_client      = utf8mb4 */;
/*!50001 SET character_set_results     = utf8mb4 */;
/*!50001 SET collation_connection      = utf8mb4_general_ci */;
/*!50001 CREATE ALGORITHM=UNDEFINED */
/*!50013 DEFINER=`root`@`localhost` SQL SECURITY DEFINER */
/*!50001 VIEW `endurance_test_data` AS select `a`.`id` AS `id`,`a`.`name` AS `name`,`a`.`size` AS `size`,`a`.`sector_size` AS `sector_size`,`b`.`cur_round_num` + `b`.`round_num_offset` AS `cur_round_num`,`b`.`num_bad_sectors` AS `num_bad_sectors`,`b`.`consolidated_sector_map` AS `consolidated_sector_map`,`b`.`status` AS `status`,`b`.`rate` AS `rate`,`b`.`last_updated` AS `last_updated` from (`cards` `a` join `consolidated_sector_maps` `b`) where `a`.`id` = `b`.`id` */;
/*!50001 SET character_set_client      = @saved_cs_client */;
/*!50001 SET character_set_results     = @saved_cs_results */;
/*!50001 SET collation_connection      = @saved_col_connection */;
/*!40103 SET TIME_ZONE=@OLD_TIME_ZONE */;

/*!40101 SET SQL_MODE=@OLD_SQL_MODE */;
/*!40014 SET FOREIGN_KEY_CHECKS=@OLD_FOREIGN_KEY_CHECKS */;
/*!40014 SET UNIQUE_CHECKS=@OLD_UNIQUE_CHECKS */;
/*!40101 SET CHARACTER_SET_CLIENT=@OLD_CHARACTER_SET_CLIENT */;
/*!40101 SET CHARACTER_SET_RESULTS=@OLD_CHARACTER_SET_RESULTS */;
/*!40101 SET COLLATION_CONNECTION=@OLD_COLLATION_CONNECTION */;
/*!40111 SET SQL_NOTES=@OLD_SQL_NOTES */;

-- Dump completed on 2024-12-24 10:32:55
